#include <sys/wait.h> //waitpid()
#include <sys/types.h> //pid_t
#include <stdio.h>      //getline()
#include <unistd.h>   //fork(),execv(),pipe
#include <iostream> //cin,cout,cerr
#include <fstream>
#include <string>     //string,substr()
#include <string.h>  //strdup
#include <vector>    //vector
#include <queue>
#include <map>
#include <set>       //set
#include <dirent.h>  //DIR,dirent,opendir
#include <fcntl.h>  //O_CREAT,O_RDWR,O_TRUNC,S_IREAD,S_IWRITE
#include <stdlib.h>
#include <sys/stat.h>//open(),S_IREAD,S_IWRITE
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/sem.h>

#define MAXLINE 15000
#define STDIN 0
#define STDOUT 1
#define STDERR 2

using namespace std;

int targetuser = -1;
const string welcome = "****************************************\n** Welcome to the information server. **\n****************************************\n";
const int maxclient = 30;
const string pa = "% ";
const int DONOTHING = 0;
const int ISBROADCAST = 1;
const int ISTELL = 2;
const int ISFIFO = 3;
const int maxnamelen = 20;
const int maxmsglen = 1024;
const key_t shmkey = 5555;
const key_t semkey = 6666;
const string upipedir="user_pipe/";//"<sender>_<receiver>"
int shmid,sem;
pid_t mypid;

map<int, int> fdmap;//key: id , value: clientfd
vector<int> cmd_queue;//user cmd長度
vector<int*> pipe_vector;//user pipe儲存處
int userpipe[31];
set <string>  Buildins; // 資料夾裡的檔案list

extern char **environ;//所有的環境變數

struct clientinfo
{
    int fd;
    char name[maxnamelen];//user name
    int msgtype;//0:nothing 1:broadcast 2:tell 
    char msg[maxmsglen];//要傳的msg
    int tellid;//要tell的目標ID
    char IP[maxmsglen]; //IP
    int Port; //Port
    pid_t pid; //mypid
    char targetfile[maxmsglen]; //sender_receive.txt
    int rec_client;
};

struct shmMSG{
    struct clientinfo infos[maxclient+1];
};
struct shmMSG *shmmsg;
//wait for [2] (lock) to equal 0 ,then increment [2] to 1 - this locks it UNDO to release the lock if processes exits before explicitly unlocking */
static struct sembuf op_lock[2] ={2,0,0,2,1,SEM_UNDO};
//decrement [1] (proc counter) with undo on exit UNDO to adjust proc counter if process exits before explicitly calling sem_close() then decrement [2] (lock) back to 0 */
static struct sembuf op_endcreate[2] ={1,-1,SEM_UNDO,2,-1,SEM_UNDO};
//decrement [1] (proc counter) with undo on exit
static struct sembuf op_open[1] ={1,-1,SEM_UNDO};
//wait for [2] (lock) to equal 0 then increment [2] to 1 - this locks it then increment [1] (proc counter)
static struct sembuf op_close[3] ={2,0,0,2,1,SEM_UNDO,1,1,SEM_UNDO};
//decrement [2] (lock) back to 0
static struct sembuf op_unlock[1] = {2,-1,SEM_UNDO};
//decrement or increment [0] with undo on exit the 30 is set to the actual amount to add or subtract (positive or negative)
static struct sembuf op_op[1] = {0, 30,SEM_UNDO};
/*Create a semaphore with a specified initial value.
If the semaphore already exists, we don't initialize it (of course).
We return the semaphore ID if all OK, else -1.*/
int sem_create(key_t key, int initval)
{//used if we create the semaphore
    // register int id, semval;
    int id, semval;
    union semun {
        int val;
        struct semid_ds *buf;
        ushort *array;
    } semctl_arg;
    if(key == IPC_PRIVATE) {
        return -1; /* not intended  for private sem */
    }else if(key == (key_t) -1 ){
        return -1; /* provaly an ftok() error by caller */
    }
    again:
        if((id = semget(key, 3, 0666 | IPC_CREAT)) <0 ){
            return -1;/* permission problem or tables full */
        }
        /*When the semaphore is created, we know that the value of all 3 members is 0.
        Get a lock on the semaphore by waiting for [2] to equal 0, then increment it.
        There is a race condition here.  There is a possibility that between the semget() above and the semop() below, 
        another process can call our sem_close() function which can remove the semaphore if that process is the last one using it.
        Therefore, we handle the error condition of an invalid semaphore ID specially below, and if it does happen, 
        we just go back and create it again.*/
        if((semop(id, &op_lock[0], 2))<0){
            if(errno == EINVAL) goto again;
                cerr<<"can't lock"<<endl;
        }
        /*Get the value of the process counter.If it equals 0, then no one has initialized the semaphore yet.*/
        if((semval = semctl(id, 1, GETVAL, 0)) <0 ){
            cerr<<"can't GETVAL"<<endl;
        }
        if(semval == 0){ /* initial state */
        /*We could initialize by doing a SETALL, but that would clear the adjust value that we set when we locked the semaphore above.  
        Instead, we'll do 2 system calls to initialize [0] and [1].*/
            semctl_arg.val = initval;
            if(semctl(id, 0, SETVAL, semctl_arg) <0 ){
                cerr<<"can't SETVAL[0] "<<endl;
            }
            semctl_arg.val = maxclient+1;/* at most are 30 client to attach this file */
            if(semctl(id, 1, SETVAL, semctl_arg) <0 ){
                cerr<<"can't SETVAL[1] "<<endl;
            }
        }
        //Decrement the process counter and then release the lock.
        if(semop(id, &op_endcreate[0], 2) <0 ){
            cerr<<"can't end create "<<endl;
        }
    return id;
}
/*Remove a semaphore.
This call is intended to be called by a server, when it is being shut down, as we do an IPC_RMID on the semaphore,
regardless whether other processes may be using it or not.
Most other processes should use sem_close() below.*/
void sem_rm(int id)
{
    if(semctl(id, 0, IPC_RMID, 0) <0)
    {
        cerr<<"can't IPC_RMID"<<endl;
    }
}
/*Open a semaphore that must already exist.
This function should be used, instead of sem_create(),
if the caller knows that the semaphore must already exist.
For example a client from a client-server pair would use this, 
if its the server's responsibility to create the semaphore.
We return the semaphore ID if all OK, else -1.*/
int sem_open(key_t key)
{
    // register int id;
    int id;
    if(key == IPC_PRIVATE) //not intended for private semaphores
    {
        return -1;
    }else if(key == (key_t) -1 ) //probably an ftok() error by caller
    {
        return -1;
    }
    if((id = semget(key, 3, 0)) <0 ) 
    {
        return -1; /* doesn't exist or tables full*/
    }
    //Decrement the process counter.  We don't need a lock to do this.
    if(semop(id, &op_open[0], 1) <0 )
    {
        cerr<<"can't open "<<endl;
    }
    return id;
}
/*Close a semaphore.
This function is for a process to call before it exits, when it is done with the semaphore.
We "decrement" the counter of processes using the semaphore, and if this was the last one, we can remove the semaphore.*/
void sem_close(int id)
{
    // register int semval;
    int semval;
    if(semop(id, &op_close[0], 3) <0 ) 
    {
        cerr<<"can't semop "<<endl;
    }
    if((semval = semctl(id, 1, GETVAL, 0)) <0) 
    {
        cerr<<"can't GETVAL"<<endl;
    }
    if(semval > maxclient+1) 
    {
        cerr<<"sem[1] > 31 "<<endl;
    }else if(semval == maxclient+1) 
    {
        sem_rm(id);
    }
    else
    {
        if(semop(id, &op_unlock[0],1) <0) 
        {
            cerr<<"can't unlock "<<endl;
        }
    }
}
//General semaphore operation.  Increment or decrement by a user-specified amount (positive or negative; amount can't be zero).
void sem_op(int id, int value)
{
    if((op_op[0].sem_op = value) == 0) 
    {
        cerr<<"can't have value == 0 "<<endl;
    }
    if(semop(id, &op_op[0], 1) <0 ) 
    {
        cerr<<"sem_op error "<<endl;
    }
}
/*Wait until a semaphore's value is greater than 0, then decrementit by 1 and return.*/
void sem_wait(int id,string func)
{
    sem_op(id, -1);
    // cout<<"Sem lock by "<<func<<endl;
}
//Increment a semaphore by 1.
void sem_signal(int id,string func)
{
    sem_op(id, 1);
    // cout<<"Sem unlock by "<<func<<endl;
}

string GetUpipeFileName(int sender,int receiver)
{
    return upipedir+to_string(sender)+"_"+to_string(receiver)+".txt";
}

void WHO(int id)
{
	string clientmsg = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
    sem_wait(sem, "WHO");
	for(int i = 1; i <= maxclient; i++)
	{
		if(shmmsg->infos[i].fd > 0)
		{
			if(i == id)
			{
				clientmsg += (to_string(i) + "\t" + strdup(shmmsg->infos[i].name) + "\t" + strdup(shmmsg->infos[i].IP) + ":" + to_string(shmmsg->infos[i].Port) + "\t<-me\n");
			}
			else
			{
				clientmsg += (to_string(i) + "\t" + strdup(shmmsg->infos[i].name) + "\t" + strdup(shmmsg->infos[i].IP) + ":" + to_string(shmmsg->infos[i].Port) + "\n");
			}
		}
	}
	write(fdmap[id], clientmsg.c_str(), clientmsg.length());
    sem_signal(sem, "WHO");
}

void Broadcast(string message, int id)
{
	sem_wait(sem, "Broadcast");
    strcpy(shmmsg->infos[id].msg,message.c_str());
    shmmsg->infos[id].msgtype=ISBROADCAST;
    sem_signal(sem,"Broadcast");
    kill(mypid,SIGUSR1);
}

void Name(string namemsg, int id)
{
	int same = 0;
    sem_wait(sem, "Name");
	for(int i = 1; i <= maxclient; i ++)
	{
		if(strdup(shmmsg->infos[i].name) == namemsg)
		{
			same = 1;
		}
	}
    string msg = "";
	if(same == 1)
	{
		msg = "*** User '" + namemsg + "' already exists. ***\n";
		write(shmmsg->infos[id].fd, msg.c_str(), msg.length());
        sem_signal(sem, "Name");
	}
	else
	{
		strcpy(shmmsg->infos[id].name, namemsg.c_str());
        string cip = strdup(shmmsg->infos[id].IP);
        string cname = strdup(shmmsg->infos[id].name);
		msg = "*** User from " + cip + ":" + to_string(shmmsg->infos[id].Port) + " is named '" + cname + "'. ***\n";
        sem_signal(sem, "Name");
		Broadcast(msg, id);
	}
}

void Yell(vector<string>cmd, int id)
{
    sem_wait(sem, "Yell");
    string msg = "";
    string cname = strdup(shmmsg->infos[id].name);
	msg = "*** " + cname + " yelled ***: ";
	cmd.erase(cmd.begin());
	for(int i = 0; i < cmd.size() - 1; i++)
	{
		msg += cmd[i] + " ";
	}
	msg += "\n";
    sem_signal(sem, "Yell");
	Broadcast(msg, id);
}

void Tell(vector<string>cmd, int id)
{
    sem_wait(sem, "Tell");
    int tellclient = stoi(cmd[1]);
    string msg = "";
	if(shmmsg->infos[tellclient].fd == -1)
	{
		msg = "*** Error: user #" + cmd[1] + " does not exist yet. ***\n";
		write(shmmsg->infos[id].fd, msg.c_str(), msg.length()); 
	}
	else
	{
		cmd.erase(cmd.begin());
		cmd.erase(cmd.begin());
        string cname = strdup(shmmsg->infos[id].name);
		msg = "*** " + cname + " told you ***: ";
		for(int i = 0; i < cmd.size() - 1; i++)
		{
			msg += cmd[i] + " ";
		}
		msg += "\n";
        strcpy(shmmsg->infos[id].msg, msg.c_str());
		shmmsg->infos[id].tellid = tellclient;
        shmmsg->infos[id].msgtype = ISTELL;
	}
    sem_signal(sem, "Tell");
	kill(mypid, SIGUSR1);
}

void CloseUnusedSocket()
{
    sem_wait(sem,"CloseUnusedSocket");
    for(int i=1;i<=maxclient;i++)
    {
        if(fdmap[i]>0)
        {//原本有連線
            if(shmmsg->infos[i].fd<0)//已經關掉了
            {
                close(fdmap[i]);//把它關掉
                fdmap[i]=-1;//更新fdmap
            }
        }
    }
    sem_signal(sem,"CloseUnusedSocket");
}

void CloseOtherSocket(int id)
{
    sem_wait(sem,"CloseOtherSocket");
    for(int i = 1; i <= maxclient; i++)
    {
        if(i != id)//不是自己
        {
            if(shmmsg->infos[i].fd>0){//user存在
                close(shmmsg->infos[i].fd);
            }
        }
    }
    sem_signal(sem,"CloseOtherSocket");
}

string read_cmdline()
{
	string arg;
	getline(cin, arg);
	return arg;
}
/* 2 */

void initial_path()
{
	setenv("PATH", "bin:." ,1);
}

void P_env(vector<string> cmd, int clientfd)
{
	char* locat;
	locat = getenv(cmd[1].c_str());
	if(locat != NULL)
	{
		cout << locat << endl;
		string env = string(locat) + "\n";
		write(clientfd, env.c_str(), env.length());
	}
}

/* 3 */
void S_env(vector<string> cmd)
{
	setenv(cmd[1].c_str() , cmd[2].c_str(), 1);
}
/**/

void ClearAllEnvironment()
{
    int i = 1;
    char *s = *environ;
    string temp;
    for (; s; i++) 
	{
		vector<string> env;
        temp=s;
		int start = 0, have_cmd = 0;
        size_t found = temp.find("=");
		if(found == string::npos)
		{
			env.push_back(temp);
		}
		else
		{
			while(1)
			{
				string cmd_content;
				if(found != string::npos)
				{
					cmd_content = temp.substr(start , found - start);
					env.push_back(cmd_content);
					start = found + 1;
					found=temp.find(" ", start);
					if(env.back() == "")
					{
						env.pop_back();
					}
				}
				else
				{
					cmd_content = temp.substr(start, temp.length() - start);
					env.push_back(cmd_content);
					if(env.back() == "")
					{
						env.pop_back();
					}
					break;
				}
			}
		}
        if(unsetenv(env[0].c_str())<0){
            cerr<<strerror(errno)<<endl;
        }
        s=*(environ+i);
    }
}

void SetBuildin()
{
	char* path;
	path = getenv("PATH");
	int start_env = 0 ;
	vector<string> loc;
	string lo;
	lo = path;
	size_t found_env = lo.find(":");
	if(found_env == string::npos)
	{
		loc.push_back(lo);
	}
	else
 	{
		while(1)
		{
			string exe_cmd;
			if(found_env != string::npos)
			{
				exe_cmd = lo.substr(start_env , found_env - start_env);
				loc.push_back(exe_cmd);
				start_env = found_env + 1;
				found_env=lo.find(":", start_env);
			}
			else
			{
				exe_cmd = lo.substr(start_env, lo.length() - start_env);
				loc.push_back(exe_cmd);
				break;
			}
		}
	}
	string temp;
	struct dirent* dp;
	DIR* dirp;
	Buildins.clear();
	if(path!=NULL)
	{
		for(int i=0;i<loc.size();i++)
		{
			dirp=opendir(loc[i].c_str());
			if(dirp)
			{
				while((dp=readdir(dirp))!=NULL)
				{
					temp.assign(dp->d_name);
					if(temp.find(".")!=string::npos)
					{
						Buildins.insert(temp);
					}
				}
			}
			closedir(dirp);  
		}
	}
}

/**/
int Outpipe(vector<int*> pipe_vector, int outpipe, int cmdnum, int g)
{
	int pipe_in_queue = cmd_queue.size();
	int loc_in_queue = -1;//PIPE到第幾個指令在queue的哪裡
	for(int a = 0; a < pipe_in_queue; a++)
	{
		if(cmd_queue[a] == outpipe)
		{
			loc_in_queue = a;
		}
	}
	return loc_in_queue;
}

int Midoutpipe(int g)
{
	int pipe_in_queue = cmd_queue.size();
	int loc_in_queue = -1;//PIPE到第幾個指令在queue的哪裡
	for(int a = 0; a < pipe_in_queue; a++)
	{
		//cerr<<cmd_queue[a]<<endl;
		if(cmd_queue[a] == (g + 2))
		{
			loc_in_queue = a;
		}
	}
	return loc_in_queue;
}

/**/
void test_close(int pipe_name)
{
	if(close(pipe_name) < 0)
	{
		cerr<<strerror(errno)<<endl;
	}
}
/**/
void test_dup2(int pipe_name1 , int pipe_name2)
{
	if(dup2(pipe_name1 , pipe_name2) < 0)
	{
		cerr<<strerror(errno)<<endl;
	}
}
/**/
void close_cmdpipe(int **cmd_pipe, vector<int> cmd_next_pipe,int g, int output)
{
	for(int j = 0 ; j < cmd_next_pipe.size() ; j ++)
	{
		if(cmd_next_pipe[j] != output && cmd_next_pipe[j] != g && cmd_next_pipe[j] != g-1)
		{
			close(cmd_pipe[cmd_next_pipe[j]][0]);
			close(cmd_pipe[cmd_next_pipe[j]][1]);
			//cerr<<"h\n";
		}
	}
}

void close_userpipe(vector<int*>userpipe, int PipeToUser, int PipeFromUser)
{
	for(int i = 0; i < userpipe.size(); i ++)
	{
		if(i != PipeToUser && i != PipeFromUser)
		{
			close(userpipe[i][0]);
			close(userpipe[i][1]);
		}
	}
}

void close_local_pipe(vector<int*> pipe_vector, int pipe_num, int input_pipe, int out_in_pipe)//把外面不要的pipe關了
{
	for(int j = 0 ; j < pipe_num ; j ++)
	{
		if(j != input_pipe && j != out_in_pipe)
		{
			close(pipe_vector[j][0]);
			close(pipe_vector[j][1]);
		}
	}
}

void CloseUserpipeLogout(int id)
{
	string upath;
    for(int i = 1; i <= maxclient; i++)
    {
        upath = GetUpipeFileName(id,i);
        if(access(upath.c_str(),F_OK)== 0)
        {
            remove(upath.c_str());
        }
        upath = GetUpipeFileName(i,id);
        if(access(upath.c_str(),F_OK)== 0)
        {
            remove(upath.c_str());
        }
    }
}

void CloseVectorPipeLogout()
{
	for(int i = 0; i < cmd_queue.size(); i++)
	{
		close(pipe_vector[i][0]);
		close(pipe_vector[i][1]);
		delete[] pipe_vector[i];
		cmd_queue.erase(cmd_queue.begin() + i);
		pipe_vector.erase(pipe_vector.begin() + i);
	}
}

/*  */
void ChildHandler(int signo)
{
    int status;
    bool isBroadcasting;
    while(waitpid(-1,&status,WNOHANG)>0){}
    while(1)
    {
        isBroadcasting=false;
        sem_wait(sem,"ChildHandler");
        for(int i=1;i<=maxclient;i++)
        {
            if(shmmsg->infos[i].msgtype!=DONOTHING)
            {
                isBroadcasting=true;
            }
        }
        sem_signal(sem,"ChildHandler");
        if(!isBroadcasting)
        {
            break;
        }
    }
    CloseUnusedSocket();
}

void MessageHandler(int s)
{
    
    switch(s)
    {
        case SIGUSR1:
            sem_wait(sem,"MessageHandler");
            for(int i=1; i<=maxclient; i++)
            {
                if(shmmsg->infos[i].msgtype==ISBROADCAST)
                {
                    for(int j=1;j<=maxclient; j++)
                    {
                        if(shmmsg->infos[j].fd>0)
                        {
                            if(write(shmmsg->infos[j].fd, shmmsg->infos[i].msg, strlen(shmmsg->infos[i].msg))<0)
                            {
                                cerr<<strerror(errno)<<endl;
                            }
                        }
                    }
                    strcpy(shmmsg->infos[i].msg, "");
                    shmmsg->infos[i].msgtype = DONOTHING;
                }
                else if(shmmsg->infos[i].msgtype==ISTELL)
                {
                    if(write(shmmsg->infos[shmmsg->infos[i].tellid].fd, shmmsg->infos[i].msg, strlen(shmmsg->infos[i].msg))<0)
                    {
                        cerr<<strerror(errno)<<endl;
                    }
                    strcpy(shmmsg->infos[i].msg, "");
                    shmmsg->infos[i].msgtype = DONOTHING;
                    shmmsg->infos[i].tellid=-1;
                }
            }
            sem_signal(sem,"MessageHandler");
        case SIGUSR2:
            sem_wait(sem, "In FIFO");
            for(int i = 1; i <= maxclient; i++)
            {
                if(shmmsg->infos[i].msgtype == ISFIFO /*&& shmmsg->infos[i].pid == mypid*/)
                {
                    int readfd = open(shmmsg->infos[i].targetfile, O_RDONLY);
                    if(readfd < 0)
                    {
                        cerr<<"can't read file\n";
                    }
                    else
                    {
                        int sender = shmmsg->infos[i].rec_client;
                        userpipe[sender] = readfd;
                        shmmsg->infos[i].msgtype = DONOTHING;
                        shmmsg->infos[i].rec_client = -1;
                        strcpy(shmmsg->infos[i].targetfile, "");
                    }
                }
            }
            sem_signal(sem, "In FIFO");
    }
}

void CloseAllUserPipe()
{
    string upath;
    for(int i = 1; i <= maxclient; i++)
    {
        for(int j = 1; j <= maxclient; j++)
        {
            upath = GetUpipeFileName(i,j);
            if(access(upath.c_str(),F_OK)==0)
            {
                remove(upath.c_str());
            }
            upath = GetUpipeFileName(j,i);
            if(access(upath.c_str(),F_OK)==0)
            {
                remove(upath.c_str());
            }
        }
    }
}

void SharedMemoryHandler(int s)
{
    CloseAllUserPipe();
    if(shmdt(shmmsg) < 0)
    {
        cerr<<"shmdt error"<<endl;
    }
    if(shmctl(shmid,IPC_RMID,(struct shmid_ds *) 0) < 0)
    {
        cerr<<"cant remove shm"<<endl;
    }
    sem_rm(sem);
    signal(SIGINT, SIG_DFL);
    raise(SIGINT);
}

/* */
int excfun(vector<string> cmd, vector<int> pipe_locate, int pipe_num_temp)
{
	char const *ex_line[1024];
	int k = 0 , i;
	for(i = pipe_locate[pipe_num_temp] + 1 ; i < pipe_locate[pipe_num_temp + 1] ; i ++)
	{
		ex_line[k] = cmd[i].c_str();
		k++;
	}
	ex_line[k] = NULL;
	if(execvp(ex_line[0], (char* const*)&ex_line) == -1)
	{
		if(!(Buildins.count(ex_line[0])))
		{
			cerr<<"Unknown command: ["<<ex_line[0]<< "]."<<endl;
		}
		else
		{
			cerr<<"Error(exec):"<<strerror(errno)<<endl;
		}
		exit(EXIT_FAILURE);
	}
}
/**/
int SetInput(int nowcommand)
{
	int pipe_in_queue = cmd_queue.size();
	int input_pipe = -1;
	for(int k = 0; k < pipe_in_queue; k ++)
	{
		int target = nowcommand + 1;
		if(cmd_queue[k] == target)
		{
			input_pipe = k;
		}
	}
	return input_pipe;
}
/**/
void ForkNewProcess(pid_t &pid)
{
    do{
        //殺殭屍
        signal(SIGCHLD,ChildHandler);
        //嘗試fork新的child process
        pid=fork();
        //如果失敗則sleep
        if(pid<0){
            usleep(1000);
        }
    }while(pid<0);
}
/* */
void SetInQueue(int next_loc, int cmdnum)//把NPIPE放進pipe_vector,cmdnum為總指令數
{
	int queue_cmd = cmd_queue.size();
	int temp = 0;
	for(int a = 0 ; a < queue_cmd; a++)
	{
		//cerr<<"cmdqueue: "<<cmd_queue[a]<<endl;
		if(cmd_queue[a] == (next_loc + cmdnum))
		{
			temp = 1;
		}
	}
	if(temp == 0)
	{
		int* N_pipe = new int[2];
		pipe(N_pipe);
		pipe_vector.push_back(N_pipe);
		cmd_queue.push_back(next_loc + cmdnum);
	}
}
/*  */
int findNpipe(vector<string> cmd, vector<int> pipe_locate, int g)
{
	string command = cmd[pipe_locate[g]];
	int cmdbehind = pipe_locate.size() - (g + 1); //還剩多少指令
	int nextcmd = 0;
	string NpipeNum = "";
	for(int v = 1 ; v < command.size() ; v ++)
	{
		NpipeNum += command[v];
	}
	if((command[0] == '|' || command[0] == '!') && command[1] != '\0' && command[1] != '1')//為|1以外的|N
	{
		nextcmd = stoi(NpipeNum);
		if(cmdbehind >= nextcmd)
		{
			return(g + nextcmd - 1); 
		}
		else
		{
			return (g + nextcmd + 1);
		}
		
	}
	else if((command[0] == '|' || command[0] == '!') && command[1] != '\0' && command[1] == '1')//|1的情況
	{
		if(g != (pipe_locate.size() - 1))//不是最後一個指令
		{
			return g;
		}
		else//最後一個指令
		{
			nextcmd = stoi(NpipeNum);
			return(g + nextcmd + 1);
		}
	}
	else 
	{
		return g;
	}
}

string SetInputFromUpipe(int sender,int receiver,string rec_cmdline)
{
    sem_wait(sem,"SetInputFromUpipe");
    string msg = "";
    if(shmmsg->infos[sender].fd<0)
    {//user 不存在
        msg="*** Error: user #"+to_string(sender)+" does not exist yet. ***\n";
        write(shmmsg->infos[receiver].fd,msg.c_str(),msg.length());
        sem_signal(sem,"SetInputFromUpipe (user not exist)");
        return "";
    }
    if(access(GetUpipeFileName(sender,receiver).c_str(),F_OK)==0)
    { //user pipe的存在
        string cname = strdup(shmmsg->infos[receiver].name);
        string frname = strdup(shmmsg->infos[sender].name);
        msg = "*** "+ cname + " (#" + to_string(receiver) + ") just received from " + frname;
        msg += " (#" + to_string(sender) + ") by '"+ rec_cmdline + "' ***\n";
        sem_signal(sem,"SetInputFromUpipe (success)");
        Broadcast(msg, receiver);
        cout<<"=====Set input from "<<GetUpipeFileName(sender,receiver)<<"====="<<endl;
        while(1){
            sem_wait(sem,"Wait for broadcast");
            if(shmmsg->infos[receiver].msgtype==DONOTHING){
                sem_signal(sem,"Wait for broadcast");
            break;
            }
            sem_signal(sem,"Wait for broadcast");
        }
        return (GetUpipeFileName(sender,receiver));
    }
    msg="*** Error: the pipe #"+to_string(sender)+"->#"+to_string(receiver)+" does not exist yet. ***\n";
    write(shmmsg->infos[receiver].fd,msg.c_str(),msg.length());
    sem_signal(sem,"SetInputFromUpipe (pipe not exist)");
    return "";
}

string SetOutputToUpipe(int sender,int receiver,string rec_cmdline)
{
    string msg="";
    sem_wait(sem,"SetOutputToUpipe");
    if(shmmsg->infos[receiver].fd<0)
    {//user不存在
        msg="*** Error: user #"+to_string(receiver)+" does not exist yet. ***\n";
        write(shmmsg->infos[sender].fd,msg.c_str(),msg.length());
        sem_signal(sem,"SetOutputToUpipe (User not exist)");
        return "";
    }
    //目前的user pipe已經有東西
    if(access(GetUpipeFileName(sender,receiver).c_str(),F_OK)==0)
    {
        msg="*** Error: the pipe #"+to_string(sender)+"->#"+to_string(receiver)+" already exists. ***\n";
        write(shmmsg->infos[sender].fd,msg.c_str(),msg.length());
        sem_signal(sem,"SetOutputToUpipe (already exist)");
        
        return "";
    }
    string cname = strdup(shmmsg->infos[sender].name);
    string toname = strdup(shmmsg->infos[receiver].name);
    msg = "*** " + cname + " (#" + to_string(sender) + ") just piped '";
    msg += rec_cmdline;
    msg += "' to " + toname + " (#" + to_string(receiver) + ") ***\n";
    sem_signal(sem,"SetOutputToUpipe (new upipe)");
    cout<<"=====Set out to "<<GetUpipeFileName(sender,receiver)<<"====="<<endl;
    Broadcast(msg, sender);
    while(1){
        sem_wait(sem,"Wait for broadcast");
        if(shmmsg->infos[sender].msgtype==DONOTHING){
            sem_signal(sem,"Wait for broadcast");
            break;
        }
        sem_signal(sem,"Wait for broadcast");
    }
    return (GetUpipeFileName(sender,receiver));
}

/*  */
int number_pipe(vector<string> cmd, vector<int> pipe_locate,vector<string> recvec,vector<int> recloc, int file, int id, int clientfd,int eatuserpipe, int PipeFromUser, int PipeToUser)
{
    //cerr<<"im in\n"<<eatuserpipe<<endl;
    string rec_cmdline = "";
    for(int i = 0 ; i < recvec.size()-1 ; i ++)
    {
        rec_cmdline += recvec[i];
        if(i != recvec.size() - 2)
        {
            rec_cmdline += " ";
        }
    }
    if((recvec[recvec.size()-1][0] == '|' || recvec[recvec.size()-1][0] == '!') && recvec[recvec.size()-1][1] !='\0')
    {
        rec_cmdline += " " + recvec[recvec.size() - 1];
    }
    string recsource = "";
    string sendsource = "";
    int writefd;
    if(PipeFromUser > -1)
    {
        recsource = SetInputFromUpipe(PipeFromUser, id, rec_cmdline);
    }
    if(PipeToUser > -1)
    {
        sendsource = SetOutputToUpipe(id, PipeToUser, rec_cmdline);
        if(sendsource !="")
        {
            int f = mkfifo(sendsource.c_str(),0644);
            if(f < 0)
            {
                cerr<<"error FIFO\n"<<sendsource<<endl;
            }
            else 
            {
                sem_wait(sem, "FIFO");
                strcpy(shmmsg->infos[PipeToUser].targetfile,sendsource.c_str());
                shmmsg->infos[PipeToUser].msgtype = ISFIFO;
                shmmsg->infos[PipeToUser].rec_client = id;
                sem_signal(sem, "FIFO");
                kill(shmmsg->infos[PipeToUser].pid,SIGUSR2);
                writefd = open(sendsource.c_str(),O_WRONLY);
                //cerr<<"Dup output to "<<sendsource<<" where f = "<<f<<endl;
            }
        }
    }
	int pipe_num_temp = 0;
	queue<pid_t> child_pid;
	pid_t pid;
	//vector<vector<int>> cmd_pipe;
	int only_pipe = pipe_locate.size();
	int **cmd_pipe;
	if(file == 1)
	{
		only_pipe --;
	}
	else if(cmd[0] == "who" || cmd[0] == "name" || cmd[0] == "yell" || cmd[0] == "tell")
	{
		only_pipe = 1;
	}
	cmd_pipe = new int*[only_pipe];
	vector<int>cmd_next_pipe;
	if(cmd[0] == "printenv")
	{
		P_env(cmd, clientfd);
	}
	else if(cmd[0] == "setenv")
	{
		S_env(cmd);
	}
	else if(cmd[0] == "who")
	{
		WHO(id);
	}
	else if(cmd[0] == "name")
	{
		Name(cmd[1], id);
	}
	else if(cmd[0] == "yell")
	{
		Yell(cmd, id);
	}
	else if(cmd[0] == "tell")
	{
		Tell(cmd, id);
	}
	else
	{
		for(int g = 0; g < only_pipe; g ++)
		{
			int input_pipe = SetInput(g);
			int outpipe = findNpipe(cmd, pipe_locate, g);
			int pipe_in_queue = cmd_queue.size();
			int lastcmd = pipe_locate[g];//檢查指令是否為'|N'
			string check = cmd[lastcmd];
			int pipe_exist = 0;
			if(only_pipe > 1)
			{
				if(outpipe <= only_pipe -1)
				{
					for(int j = 0 ; j < cmd_next_pipe.size() ; j ++)
					{
						if(cmd_next_pipe[j] == g || cmd_next_pipe[j] == outpipe)
						{
							pipe_exist = 1;
						}
					}
					if(pipe_exist == 0)
					{
						if(outpipe > g)
						{
							cmd_next_pipe.push_back(outpipe);
							cmd_pipe[outpipe] = new int[2];
							pipe(cmd_pipe[outpipe]);
						}
						cmd_pipe[g] = new int[2];
						pipe(cmd_pipe[g]);
					}
				}
				else
				{
					cmd_pipe[g] = new int[2];
					pipe(cmd_pipe[g]);
				}
				
				if(g > 1)
				{
					close(cmd_pipe[g-2][0]);
					close(cmd_pipe[g-2][1]);
					delete [] cmd_pipe[g-2];
					for(int x = 0; x < cmd_next_pipe.size(); x ++)
					{
						if(cmd_next_pipe[x] == (g-2))
						{
							cmd_next_pipe.erase(cmd_next_pipe.begin() + x);
						}
					}
				}
			}
			if(g == 0)
			{
				ForkNewProcess(pid);
				if(only_pipe == 1 && check[0] != '|' && check[0] != '!' && sendsource == "")
				{
					child_pid.push(pid);
				}
				if(pid == 0)
				{
					//cerr<<"first: "<<input_pipe<<"outpipe: "<<outpipe<<endl;
					if(only_pipe > 1 && input_pipe < 0)//沒有先前的pipe而且有>一條的指令
					{
						close(STDOUT);
						if(outpipe > only_pipe -1)
						{
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g);
							//cerr<<"C1_Input: "<<input_pipe<<"outpipe: "<<outpipe<<" pipe_vector:"<<pipe_vector.size()<<"out: "<<out<<endl;
							close_local_pipe(pipe_vector, pipe_in_queue, -1, out);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(pipe_vector[out][0]);
							close(STDERR);

                            if(eatuserpipe == g)
                            {
                                close(STDIN);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							if(check[0] == '!')
							{
								dup2(pipe_vector[out][1], STDERR);
							}
							else
							{
								dup2(clientfd, STDERR);
							}
							dup2(pipe_vector[out][1], STDOUT);	
						}
						else
						{
							//cerr<<"C2_Input: "<<input_pipe<<"outpipe: "<<outpipe<<" nextcmd_vector:"<<cmd_next_pipe.size()<<endl;
							int midout = Midoutpipe(g);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, midout); //把其他PIPE關起來
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, outpipe);
							if(g != outpipe)
							{
								close(cmd_pipe[g][0]);
								close(cmd_pipe[g][1]);
							}
							close(cmd_pipe[outpipe][0]);
							close(STDERR);

							if(eatuserpipe == g)
                            {
                                close(STDIN);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							if(midout >= 0)
							{
							    close(cmd_pipe[outpipe][1]);
								close(pipe_vector[midout][0]);
								dup2(pipe_vector[midout][1], STDOUT);
							}
							else
							{
								dup2(cmd_pipe[outpipe][1], STDOUT);
							}
							if(check[0] == '!')
							{
								if(midout >= 0)
								{
									dup2(pipe_vector[midout][1], STDERR);
								}
								else
								{
									dup2(cmd_pipe[outpipe][1], STDERR);
								}
							}
							else
							{
								dup2(clientfd, STDERR);
							}
						}
					}
					else if(only_pipe > 1 && input_pipe >= 0)//有先前的pipe而且有>一個的指令
					{
						if(outpipe > only_pipe -1)//|N, N>指令數
						{
							//cerr<<"C3\n";
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out);
							close(STDOUT);
							close(STDIN);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(pipe_vector[input_pipe][1]);
							close(pipe_vector[out][0]);
							close(STDERR);
							
							if(check[0] == '!')
							{
								dup2(pipe_vector[out][1], STDERR);
							}
							else
							{
								dup2(clientfd, STDERR);
							}
							dup2(pipe_vector[input_pipe][0], STDIN);
							dup2(pipe_vector[out][1], STDOUT);
						}
						else
						{
							//cerr<<"C4: "<<input_pipe<<"outpipe: "<<outpipe<<" pipe_vector:"<<pipe_vector.size();
							int midout = Midoutpipe(g);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, midout); //把其他PIPE關起來
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, outpipe);
							close(STDOUT);
							close(STDIN);
							if(g != outpipe)
							{
								close(cmd_pipe[g][0]);
								close(cmd_pipe[g][1]);
							}
							close(pipe_vector[input_pipe][1]);
							close(cmd_pipe[outpipe][0]);
							close(STDERR);
							
							if(midout >= 0)
							{
							    close(cmd_pipe[outpipe][1]);
								close(pipe_vector[midout][0]);
								dup2(pipe_vector[midout][1], STDOUT);
							}
							else
							{
								dup2(cmd_pipe[outpipe][1], STDOUT);
							}
							if(check[0] == '!')
							{
								if(midout >= 0)
								{
									dup2(pipe_vector[midout][1], STDERR);
								}
								else
								{
									dup2(cmd_pipe[outpipe][1], STDERR);
								}
							}
							else
							{
								dup2(clientfd, STDERR);
							}
							dup2(pipe_vector[input_pipe][0], STDIN);
						}
					}
					else if(only_pipe == 1 && input_pipe < 0)//只有一個指令&沒有之前的PIPE
					{	
						if(file == 1)//這條指令是寫檔
						{
							//cerr<<"C5_Input: "<<input_pipe<<" outpipe: "<<outpipe<<" pipe_vector: "<<pipe_vector.size()<<endl;
							close_local_pipe(pipe_vector, pipe_in_queue, -1, -1);
							int file = open(cmd[cmd.size() - 2].c_str(), O_CREAT|O_RDWR|O_TRUNC,S_IREAD|S_IWRITE);
							close(STDOUT);
							close(STDERR);
                            if(eatuserpipe == g)
                            {
                                close(STDIN);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							dup2(clientfd, STDERR);
							test_dup2(file, STDOUT);
							close(file);
						}
						else if(outpipe > only_pipe -1)
						{
							test_close(STDOUT);
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g);
							//cerr<<"C6_Input: "<<input_pipe<<" outpipe: "<<outpipe<<" pipe_vector: "<<pipe_vector.size()<<" out: "<<out<<endl;
							close_local_pipe(pipe_vector, pipe_in_queue, -1, out);
							close(pipe_vector[out][0]);
							close(STDERR);
							if(eatuserpipe == g)
                            {
                                close(STDIN);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
                            dup2(pipe_vector[out][1], STDOUT);
							if(check[0] == '!')
							{
								test_dup2(pipe_vector[out][1], STDERR);
							}
							else
							{
								dup2(clientfd, STDERR);
							}
							
						}
						else
						{
							//cerr<<outpipe<<"C7: "<<input_pipe<<" outpipe: "<<outpipe<<" pipe_vector: "<<pipe_vector.size()<<endl;
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							close_local_pipe(pipe_vector, pipe_in_queue, -1, -1);
							close(STDOUT);
                            //cerr<<"baga: " <<eatuserpipe<<" ok: " << g<<endl;
							if(eatuserpipe == g)
                            {
                                close(STDIN);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							if(PipeToUser > -1 && sendsource != "")
                            {
                                dup2(writefd,STDOUT);
                                close(writefd);
                            }
                            else if(PipeToUser > -1 && sendsource == "")
                            {
                                int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDOUT);
                            }
                            else
                            {
                                dup2(clientfd, STDOUT);
                            }
                            close(STDERR);
                            dup2(clientfd, STDERR);
						}
					}
					else if(only_pipe == 1 && input_pipe >= 0) //只有一個指令&有先前PIPE
					{
						if(file == 1)//這條指令是寫檔
						{
							//cerr<<"C8\n";
							int file = open(cmd[cmd.size() - 2].c_str(), O_CREAT|O_RDWR|O_TRUNC,S_IREAD|S_IWRITE);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1); //把其他PIPE關起來
							close(STDOUT);
							close(STDIN);
							close(pipe_vector[input_pipe][1]);
							dup2(pipe_vector[input_pipe][0], STDIN);
							dup2(file, STDOUT);
							close(STDERR);
							
							dup2(clientfd, STDERR);
							close(file);
						}
						else if(outpipe > g)
						{
							//cerr<<"C9\n";
							close(STDOUT);
							close(STDIN);
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out);
							close(pipe_vector[input_pipe][1]);
							close(pipe_vector[out][0]);
							close(STDERR);
							
							if(check[0] == '!')
							{
								test_dup2(pipe_vector[out][1], STDERR);
							}
							else
							{
								dup2(clientfd, STDERR);
							}
							dup2(pipe_vector[input_pipe][0], STDIN);
							dup2(pipe_vector[out][1], STDOUT);
						}
						else
						{
							//cerr<<"C10_input: "<<input_pipe<<"outpipe: "<<outpipe<<"cmd_queue:"<<cmd_queue.size()<<endl;
							close(STDIN);
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1);
							close(pipe_vector[input_pipe][1]);
							close(STDOUT);
							close(STDERR);
							
							dup2(clientfd, STDERR);
							if(PipeToUser > -1 && sendsource != "")
                            {
                                dup2(writefd,STDOUT);
                                close(writefd);
                            }
                            else if(PipeToUser > -1 && sendsource == "")
                            {
                                int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDOUT);
                            }
                            else
                            {
                                dup2(clientfd, STDOUT);
                            }
							dup2(pipe_vector[input_pipe][0], STDIN);
						}
					}
					char const *ex_line[1024];
					int k = 0 , i;
					for(i = 0 ; i < pipe_locate[0] ; i ++)
					{
						ex_line[k] = cmd[i].c_str();
						//cerr<<ex_line[k]<<endl;
						k++;
					}
					ex_line[k] = NULL;
					if(execvp(ex_line[0], (char* const*)&ex_line) == -1)
					{
						if(!(Buildins.count(ex_line[0])))
						{
							cerr<<"Unknown command: ["<<ex_line[0]<< "]."<<endl;
						}
						else
						{
							cerr<<"Error(exec):"<<strerror(errno)<<endl;
						}
						exit(EXIT_FAILURE);
					}
				}	
			}
			else
			{
				ForkNewProcess(pid);
				if(g == only_pipe - 1 && check[0] != '|' && check[0] != '!' && sendsource == "")
				{
					child_pid.push(pid);
				}
				if (pid == 0)
				{
					if(g == only_pipe - 1 && check[0] != '|' && check[0] != '!')//最後一個指令不是'|N' or '!N'
					{
						if(file == 1)
						{
							//cerr<<"C11\n";
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1);
							int file = open(cmd[cmd.size() - 2].c_str(), O_CREAT|O_RDWR|O_TRUNC,S_IREAD|S_IWRITE);
							close(STDIN);
							close(STDOUT);
							close(cmd_pipe[g-1][1]);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(STDERR);
							if(eatuserpipe == g)
                            {
                                close(cmd_pipe[g-1][0]);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							if(input_pipe >= 0)
							{
								close(pipe_vector[input_pipe][1]);
								close(cmd_pipe[g-1][0]);
								dup2(pipe_vector[input_pipe][0] , STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
                            dup2(clientfd, STDERR);
							dup2(file, STDOUT);
							close(file);
						}
						else
						{
							// cerr<<"C12_INput: "<<input_pipe<<" outpipe: "<<outpipe<<" cmd_queue_size: "<<cmd_queue.size()<<endl;
							close(STDIN);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(cmd_pipe[g-1][1]);
							close(STDOUT);
							close(STDERR);
							if(eatuserpipe == g)
                            {
                                close(cmd_pipe[g-1][0]);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							if(input_pipe >= 0)
							{
								close(pipe_vector[input_pipe][1]);
								close(cmd_pipe[g-1][0]);
								dup2(pipe_vector[input_pipe][0] , STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
                            if(PipeToUser > -1 && sendsource != "")
                            {
                                dup2(writefd,STDOUT);
                                close(writefd);
                            }
                            else if(PipeToUser > -1 && sendsource == "")
                            {
                                int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDOUT);
                            }
                            else
                            {
                                dup2(clientfd, STDOUT);
                            }
							dup2(clientfd, STDERR);
						}
					}
					else if(g == only_pipe - 1 && check[0] == '|')//最後一個指令是'|N'
					{
						close(STDOUT);
						close(STDIN);
						int out = Outpipe(pipe_vector, outpipe, only_pipe, g);
						//cerr<<"C13_INput: "<<input_pipe<<" outpipe: "<<outpipe<<" cmd_queue_size: "<<cmd_queue.size()<<" out: "<<out<<endl;
						close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out);
						close(cmd_pipe[g-1][1]);
						test_close(cmd_pipe[g][0]);
						test_close(cmd_pipe[g][1]);
						close(pipe_vector[out][0]);
						close(STDERR);
						if(eatuserpipe == g)
                        {
                            close(cmd_pipe[g-1][0]);
                            if(recsource == "")
                            {
                                int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
                            }
                            else
                            {
                                int f = userpipe[PipeFromUser];
                                //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                dup2(f,STDIN);
                                close(f);
                            }
                        }
                        if(input_pipe >= 0)
						{
							close(pipe_vector[input_pipe][1]);
							close(cmd_pipe[g-1][0]);
							dup2(pipe_vector[input_pipe][0] , STDIN);
						}
						else
						{
							dup2(cmd_pipe[g-1][0] , STDIN);
						}
						dup2(clientfd, STDERR);
						dup2(pipe_vector[out][1], STDOUT);
					}
					else if(g == only_pipe - 1 && check[0] == '!') //最後一個指令是'!N'
					{
						close(STDOUT);
						close(STDIN);
						close(STDERR);
						int out = Outpipe(pipe_vector, outpipe, only_pipe, g);
						//cerr<<"C14_INput: "<<input_pipe<<" outpipe: "<<outpipe<<" cmd_queue_size: "<<cmd_queue.size()<<" out: "<<out<<endl;
						close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out);
						close(cmd_pipe[g-1][1]);
						close(pipe_vector[out][0]);
						close(cmd_pipe[g][0]);
						close(cmd_pipe[g][1]);
						if(eatuserpipe == g)
                        {
                            close(cmd_pipe[g-1][0]);
                            if(recsource == "")
                            {
                                int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
                            }
                            else
                            {
                                int f = userpipe[PipeFromUser];
                                //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                dup2(f,STDIN);
                                close(f);
                            }
                        }
                        if(input_pipe >= 0)
						{
							close(pipe_vector[input_pipe][1]);
							close(cmd_pipe[g-1][0]);
							dup2(pipe_vector[input_pipe][0] , STDIN);
						}
						else
						{
							dup2(cmd_pipe[g-1][0] , STDIN);
						}
						dup2(pipe_vector[out][1], STDERR);
						dup2(pipe_vector[out][1], STDOUT);
					}
					else //中間指令
					{
						if(outpipe > only_pipe -1)
						{
							//cerr<<"C15\n";
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out);
							close(STDOUT);
							close(STDIN);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(cmd_pipe[g-1][1]);
							close(pipe_vector[out][0]);
							close(STDERR);
							if(eatuserpipe == g)
                            {
                                close(cmd_pipe[g-1][0]);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							if(input_pipe >= 0)
							{
								close(pipe_vector[input_pipe][1]);
								close(cmd_pipe[g-1][0]);
								dup2(pipe_vector[input_pipe][0] , STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							dup2(clientfd, STDERR);
							dup2(pipe_vector[out][1], STDOUT);
						}
						else
						{
							//cerr<<"C16: "<<input_pipe<<"outpipe: "<<outpipe<<"cmd_queue:"<<cmd_queue.size()<<endl;
							int midout = Midoutpipe(g);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, midout); //把其他PIPE關起來
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, outpipe);
							close(STDOUT);
							close(STDIN);
							close(cmd_pipe[g-1][1]);
							close(cmd_pipe[outpipe][0]);
							close(STDERR);
							if(eatuserpipe == g)
                            {
                                close(cmd_pipe[g-1][0]);
                                if(recsource == "")
                                {
                                    int nul = open("/dev/null", O_RDWR);
                                    dup2(nul, STDIN);
                                }
                                else
                                {
                                    int f = userpipe[PipeFromUser];
                                    //cerr<<"Dup input from "<<recsource<<" where f = "<<f<<endl;
                                    dup2(f,STDIN);
                                    close(f);
                                }
                            }
							if(input_pipe >= 0)
							{
								close(pipe_vector[input_pipe][1]);
								close(cmd_pipe[g-1][0]);
								dup2(pipe_vector[input_pipe][0] , STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							if(midout >= 0)
							{
							    close(cmd_pipe[outpipe][1]);
								close(pipe_vector[midout][0]);
								dup2(pipe_vector[midout][1], STDOUT);
								if(check[0] == '!')
								{
									dup2(pipe_vector[midout][1], STDERR);
								}
								else
								{
									dup2(clientfd, STDERR);
								}
							}
							else
							{
								if(check[0] == '!')
								{
									dup2(cmd_pipe[outpipe][1], STDERR);
								}
								else
								{
									dup2(clientfd, STDERR);
								}
								dup2(cmd_pipe[outpipe][1], STDOUT);
							}
						}
					}
					excfun(cmd, pipe_locate, pipe_num_temp);
				}
				pipe_num_temp ++;
			}
        //cerr<<"ok?\n";
		}//for迴圈尾
	}
	if(only_pipe >= 2)
	{
        //cerr<<"close_pipe?\n";
		test_close(cmd_pipe[only_pipe-1][0]);
		test_close(cmd_pipe[only_pipe-1][1]);
		test_close(cmd_pipe[only_pipe-2][0]);
		test_close(cmd_pipe[only_pipe-2][1]);
		delete[] cmd_pipe[only_pipe-1];
		delete[] cmd_pipe[only_pipe-2];
	}
	
	for(int y = 0; y < cmd_queue.size(); y ++)
	{
		cmd_queue[y] -= only_pipe;
	}
	for(int r = 0 ; r < cmd_queue.size(); r++)
	{
        //cerr<<"vector?\n";
		if(cmd_queue[r] <= 0)
		{
			close(pipe_vector[r][0]);
			close(pipe_vector[r][1]);
			delete[] pipe_vector[r];
			pipe_vector.erase(pipe_vector.begin() + r);
			cmd_queue.erase(cmd_queue.begin() + r);
		}
	}
    if(PipeToUser != -1)
    {
        close(writefd);
    }	

    if(PipeFromUser != -1)
    {
        close(userpipe[PipeFromUser]);
        unlink(recsource.c_str());
        userpipe[PipeFromUser] = -1;
    }

	while(!(child_pid.empty()))
	{
		int status;
		waitpid(child_pid.front(),&status,0);
		child_pid.pop();
	}
}
/*  */
int exe_cmd(vector<string> cmd, vector<int> pipe_locate,vector<string> recvec,vector<int> recloc, int id,int clientfd, int eatuserpipe, int PipeFromUser, int PipeToUser)
{
	int fi = 0;
	int only_pipe = pipe_locate.size();
	if(only_pipe != 1)
	{
		if(cmd[pipe_locate[only_pipe - 2]] == ">")
		{
			fi = 1;
		}
	}
	number_pipe(cmd, pipe_locate, recvec, recloc, fi, id, clientfd, eatuserpipe, PipeFromUser, PipeToUser);
	return 1;
}

/*  */
int cmp_cmdline(string arg, int id, int clientfd) // separate string
{
	vector<int> pipe_locate;
    int start = 0, have_cmd = 0;
	int PipeToUser = -1, PipeFromUser = -1;
	int eatuserpipe = -1;
	string recmsg = "";
	string semsg = "";
	vector<string> cmd; //command in line
	vector<string> recvec; //存完整cmd
	vector<int> recloc; //recvec的PIPE位置
    size_t found = arg.find(" ");
    if(found == string::npos)
	{
		cmd.push_back(arg);
		recvec.push_back(arg);
	}
	else
 	{
		while(1)
		{
			string cmd_content;
			if(found != string::npos)
			{
				cmd_content = arg.substr(start , found - start);
				cmd.push_back(cmd_content);
				recvec.push_back(cmd_content);
				start = found + 1;
				found=arg.find(" ", start);
				if(cmd.back() == "")
				{
					cmd.pop_back();
					recvec.pop_back();
				}
				else if(cmd.back() == "|" || cmd.back() == ">" || cmd.back() == "!")
				{
					pipe_locate.push_back(cmd.size() - 1);
					recloc.push_back(recvec.size()-1);
				}
				else if((cmd[cmd.size()-1][0] == '|' || cmd[cmd.size()-1][0] == '!') && cmd[cmd.size()-1][1] != '\0')
				{
					pipe_locate.push_back(cmd.size() - 1);
					recloc.push_back(recvec.size()-1);
				}
				else if(cmd[cmd.size()-1][0] == '>' && cmd[cmd.size()-1][1] != '\0')
				{
                    string nextou = recvec[recvec.size()-1];
                    string tmp = "";
                    for(int b = 1; b < nextou.size(); b++)
                    {
                        tmp += nextou[b];
                    }
					PipeToUser = stoi(tmp);
					cmd.pop_back();
				}
				else if(cmd[cmd.size()-1][0] == '<' && cmd[cmd.size()-1][1] != '\0')
				{
                    string nextou = recvec[recvec.size()-1];
                    string tmp = "";
                    for(int b = 1; b < nextou.size(); b++)
                    {
                        tmp += nextou[b];
                    }
					PipeFromUser = stoi(tmp);
					eatuserpipe = pipe_locate.size();
					cmd.pop_back();
				}
			}
			else
			{
				cmd_content = arg.substr(start, arg.length() - start);
				cmd.push_back(cmd_content);
				recvec.push_back(cmd_content);
				if(cmd.back() == "")
				{
					cmd.pop_back();
					recvec.pop_back();
				}
				else if(cmd.back() == "|" || cmd.back() == ">" || cmd.back() == "!")
				{
					pipe_locate.push_back(cmd.size() - 1);
					recloc.push_back(recvec.size()-1);
				}
				else if((cmd[cmd.size()-1][0] == '|' || cmd[cmd.size()-1][0] == '!') && cmd[cmd.size()-1][1] != '\0')
				{
					pipe_locate.push_back(cmd.size() - 1);
					recloc.push_back(recvec.size()-1);
				}
				else if(cmd[cmd.size()-1][0] == '>' && cmd[cmd.size()-1][1] != '\0')
				{
					string nextou = recvec[recvec.size()-1];
                    string tmp = "";
                    for(int b = 1; b < nextou.size(); b++)
                    {
                        tmp += nextou[b];
                    }
                    PipeToUser = stoi(tmp);
					cmd.pop_back();
				}
				else if(cmd[cmd.size()-1][0] == '<' && cmd[cmd.size()-1][1] != '\0')
				{
					string nextou = recvec[recvec.size()-1];
                    string tmp = "";
                    for(int b = 1; b < nextou.size(); b++)
                    {
                        tmp += nextou[b];
                    }
					PipeFromUser = stoi(tmp);
					eatuserpipe = pipe_locate.size();
					cmd.pop_back();
				}
				break;
			}
		}
	}
	string lastcmd = cmd.back();
	int cmdnum = pipe_locate.size();
	if(lastcmd[0] != '|' && lastcmd[0] != '!')
	{
		cmdnum ++;
	}
	for(int i = 0; i < pipe_locate.size(); i ++)
	{
		string checkNpipe = cmd[pipe_locate[i]];
		string NpipeNum = "";
		for(int v = 1 ; v < checkNpipe.size() ; v ++)
		{
			NpipeNum += checkNpipe[v];
		}
		if((checkNpipe[0] == '|' || checkNpipe[0] == '!') && checkNpipe[1] != '\0')
		{
			int next_loc = 0;
			int numpipe = stoi(NpipeNum);
			if(lastcmd[0] != '|' && lastcmd[0] != '!')
			{
				next_loc =  numpipe - (pipe_locate.size() - i);
			}
			else if(lastcmd[0] == '|' || lastcmd[0] == '!')
			{
				next_loc = numpipe - (pipe_locate.size() - i - 1);
			}
			if(next_loc > 0)
			{
				SetInQueue(next_loc, cmdnum);
			}
		}
	}
	if(lastcmd[0] != '|' && lastcmd[0] != '!')
	{
		pipe_locate.push_back(cmd.size()); //結尾位子
		recloc.push_back(recvec.size());
		cmd.push_back(to_string(cmd.size()));
		recvec.push_back(to_string(recvec.size()));
	}
	if(cmd[0]!= "")
	{
		exe_cmd(cmd, pipe_locate, recvec, recloc, id, clientfd, eatuserpipe, PipeFromUser, PipeToUser);
	}
	return 1;
}

int main(int argc, char **argv)
{
    pid_t pid;
	int serverfd, clientfd;
    int client_id = 0;
    struct sockaddr_in  server, client;
    char buff[MAXLINE];
    int  n;
	int yes=1; // 供底下的 setsockopt() 設定 SO_REUSEADDR
	socklen_t clientSize = sizeof(client);
    mypid=getpid();
    signal(SIGINT, SharedMemoryHandler);
    signal(SIGUSR1, MessageHandler);
    if ((shmid=shmget(shmkey,sizeof(struct shmMSG),IPC_CREAT | 0666))<0) 
    {
        cerr<<"shmget error : "<<strerror(errno)<<endl;
        exit(EXIT_FAILURE);
    }
    shmmsg = (struct shmMSG*) shmat(shmid, (char *)0, 0);
    if ((sem = sem_create(semkey, 1)) < 0)
    {
        cerr<<"sem_create error"<<endl;
    }
    if ((sem = sem_open(semkey)) < 0)
    {
        cerr<<"sem open error"<<endl;
    }
    sem_wait(sem,"initial");
    for(int i=1;i<=maxclient;i++)
    {
        shmmsg->infos[i].fd=-1;//設定fd的初始值
        fdmap[i]=-1;
        strcpy(shmmsg->infos[i].name,"");//設定name的初始值
        shmmsg->infos[i].msgtype=DONOTHING;
    }
    for(int i = 1; i <= maxclient;i++)
    {
        shmmsg->infos[i].fd=-1;//設定fd的初始值
        fdmap[i]=-1;
        strcpy(shmmsg->infos[i].name,"");//設定name的初始值
        shmmsg->infos[i].msgtype=DONOTHING;
    }
    sem_signal(sem,"initial");
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(atoi(argv[1]));
	if( (serverfd = socket(AF_INET, SOCK_STREAM, 0)) == -1 )
    {
        printf("create socket error: %s(errno: %d)\n",strerror(errno),errno);
        return 0;
    }
	setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
    if(bind(serverfd, (struct sockaddr*)&server, sizeof(server)) == -1)
    {
        printf("bind socket error: %s(errno: %d)\n",strerror(errno),errno);
        return 0;
	}
    if(listen(serverfd, 100) == -1)
    {
        printf("listen socket error: %s(errno: %d)\n",strerror(errno),errno);
        return 0;
    }
	while(1)
    {
        clientfd = accept(serverfd,(struct sockaddr*) &client, &clientSize);
        cout<<"accept: "<<clientfd<<endl;
        if(clientfd < 0)
        {
            cerr<<"Accept error: "<<strerror(errno)<<endl;
        }
        else//設定new client ID
        {
            sem_wait(sem,"Setting new client");
            for(int j = 1; j <= maxclient; j++)
            {
                if(shmmsg->infos[j].fd==-1)
                {
                    for(int a = 0; a < 31 ; a++)
                    {
                        userpipe[a] = -1;
                    }
                    cout<<"-----New user : ID="<<j<<"-----"<<endl;
                    client_id=j;
                    shmmsg->infos[j].fd=clientfd;
                    fdmap[j]=clientfd;
                    strcpy(shmmsg->infos[j].name,"(no name)");
                    write(shmmsg->infos[j].fd,welcome.c_str(),welcome.length());
                    string msg="*** User '";
                    msg+=shmmsg->infos[j].name;
                    strcpy(shmmsg->infos[j].IP, inet_ntoa(client.sin_addr));
                    shmmsg->infos[j].Port = ntohs(client.sin_port);
                    string cip = strdup(shmmsg->infos[j].IP);
                    msg += "' entered from " + cip + ":" + to_string(shmmsg->infos[j].Port) + ". ***\n";
                    sem_signal(sem,"Setting new client (log in msg)");
                    Broadcast(msg, client_id);
                    sem_wait(sem,"Setting new client (write prompt)");
                    write(shmmsg->infos[j].fd,pa.c_str(),pa.length());
                    break;
                }
            }
            sem_signal(sem,"Setting new client");
        }
        ForkNewProcess(pid);
        if(pid == 0)
        {
            pid_t pidname;
            pidname = getpid();
            shmmsg->infos[client_id].pid = pidname;
            CloseOtherSocket(client_id);
            ClearAllEnvironment();//移除所有環境變數
            initial_path();//設定PATH
            while(1)
            {
                signal(SIGUSR2, MessageHandler);
                SetBuildin();
                string line = "";
                int state = 1;
                memset (&buff, 0, MAXLINE);
                do
                {
                    n = read(clientfd, buff, sizeof(buff));
                    line += buff;
                }while(line.find("\n") == string::npos);
                if(line.find("\n")!=string::npos){
                    line=line.substr(0,line.length()-1);
                }
                if(line.find("\r")!=string::npos){
                    line=line.substr(0,line.length()-1);
                }
                cout<<"recv:["<<line<<"]"<<endl;
                if(line == "exit")
                {
                    sem_wait(sem, "Log out msg");
                    string exitmsg="*** User '";
                    exitmsg+=shmmsg->infos[client_id].name;
                    exitmsg+="' left. ***\n";
                    sem_signal(sem,"Log out msg");
                    Broadcast(exitmsg, client_id);
                    close(shmmsg->infos[client_id].fd);
                    CloseUserpipeLogout(client_id);
                    CloseVectorPipeLogout();
                    while(1)//防止%印在Broadcast前面
                    {
                        sem_wait(sem,"Wait for broadcast");
                        if(shmmsg->infos[client_id].msgtype==DONOTHING){
                            sem_signal(sem,"Wait for broadcast");
                            break;
                        }
                        sem_signal(sem,"Wait for broadcast");
                    }
                    sem_wait(sem,"Remove User");
                    shmmsg->infos[client_id].fd = -1;//將此id設為可用
                    shmmsg->infos[client_id].msgtype=DONOTHING;
                    strcpy(shmmsg->infos[client_id].name,"");//移除此user的Nickname
                    strcpy(shmmsg->infos[client_id].IP,"");//移除此user的IP
                    shmmsg->infos[client_id].Port = -1;
					shmmsg->infos[client_id].pid = 0;
                    sem_signal(sem,"Remove User");
                    cout<<"-----User "<<client_id<<" logout-----"<<endl;
                    exit(EXIT_SUCCESS);
                }
                else if(line != "")
                {
                    state = cmp_cmdline(line, client_id, clientfd);
                    while(1)
                    {//防止%印在Broadcast前面
                        sem_wait(sem,"Wait for broadcast");
                        if(shmmsg->infos[client_id].msgtype==DONOTHING){
                            sem_signal(sem,"Wait for broadcast");
                            break;
                        }
                        sem_signal(sem,"Wait for broadcast");
                    }
                    write(clientfd, pa.c_str(), pa.length());
                }
                else if(line == "")
                {
                    while(1)
                    {//防止%印在Broadcast前面
                        sem_wait(sem,"Wait for broadcast");
                        if(shmmsg->infos[client_id].msgtype==DONOTHING){
                            sem_signal(sem,"Wait for broadcast");
                            break;
                        }
                        sem_signal(sem,"Wait for broadcast");
                    }
                    write(clientfd, pa.c_str(), pa.length());
                }
            }
        }
    } // END got new incoming connection
	return 0;
}