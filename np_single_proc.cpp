#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <queue>
#include <fcntl.h>
#include <vector>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <set>       //set
#include<map>
#include <arpa/inet.h>
#include<netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>

#define MAXLINE 15000
#define STDIN 0
#define STDOUT 1
#define STDERR 2

using namespace std;

const string welcome = "****************************************\n** Welcome to the information server. **\n****************************************\n";
const int clientmax = 30;
const string pa = "% ";

fd_set master; // master file descriptor 清單
map<int, int> fdmap;//key: id , value: clientfd
map<int, string> client_name;//key: id, value: name
map<int,map<string,string>> clientenv; //key:id value:<key:name value :value>
map<int, string> clientIP;
map<int, int> clientPORT;
vector<int> cmd_queue[31];//user cmd長度
vector<int*> pipe_vector[31];//user pipe儲存處
vector<int> sender;
vector<int> receiver;
vector<int*> userpipe;
set <string>  Buildins; // 資料夾裡的檔案list

/* 1 */

void WHO(int id)
{
	string clientmsg = "<ID>\t<nickname>\t<IP:port>\t<indicate me>\n";
	for(int i = 1; i <= clientmax; i++)
	{
		if(fdmap[i] > 0)
		{
			if(i == id)
			{
				clientmsg += (to_string(i) + "\t" + client_name[i] + "\t" + clientIP[i] + ":" + to_string(clientPORT[i]) + "\t<-me\n");
			}
			else
			{
				clientmsg += (to_string(i) + "\t" + client_name[i] + "\t" + clientIP[i] + ":" + to_string(clientPORT[i]) + "\n");
			}
		}
	}
	write(fdmap[id], clientmsg.c_str(), clientmsg.length());
}

void Broadcast(string msg)
{
	for(int j = 1; j <= clientmax; j++)
	{
    	// 送給大家！
		if(fdmap[j] > 0)
		{
			if (FD_ISSET(fdmap[j], &master))
			{
				write(fdmap[j], msg.c_str(), msg.length());
			}
		}
    }
}

void Name(string namemsg, int id)
{
	int same =0;
	for(int i = 1; i <= 30; i ++)
	{
		if(client_name[i] == namemsg)
		{
			same = 1;
		}
	}
	if(same == 1)
	{
		string msg = "*** User '" + namemsg + "' already exists. ***\n";
		write(fdmap[id], msg.c_str(), msg.length());
	}
	else
	{
		client_name[id] = namemsg;
		string msg = "*** User from " + clientIP[id] + ":" + to_string(clientPORT[id]) + " is named '" + client_name[id] + "'. ***\n";
		Broadcast(msg);
	}
}

void Yell(vector<string>cmd, int id)
{
	string msg = "*** " + client_name[id] + " yelled ***: ";
	cmd.erase(cmd.begin());
	for(int i = 0; i < cmd.size() - 1; i++)
	{
		msg += cmd[i] + " ";
	}
	msg += "\n";
	Broadcast(msg);
}

void Tell(vector<string>cmd, int id)
{
	int tellclient = stoi(cmd[1]);
	if(fdmap[tellclient] == -1)
	{
		string msg = "*** Error: user #" + cmd[1] + " does not exist yet. ***\n";
		write(fdmap[id], msg.c_str(), msg.length());
	}
	else
	{
		cmd.erase(cmd.begin());
		cmd.erase(cmd.begin());
		string msg = "*** " + client_name[id] + " told you ***: ";
		for(int i = 0; i < cmd.size() - 1; i++)
		{
			msg += cmd[i] + " ";
		}
		msg += "\n";
		write(fdmap[tellclient], msg.c_str(), msg.length());
	}

}

string read_cmdline()
{
	string arg;
	getline(cin, arg);
	return arg;
}
/* 2 */
void P_env(vector<string> cmd, int id)
{
	char* locat;
	locat = getenv(cmd[1].c_str());
	if(locat != NULL)
	{
		cout << locat << endl;
		string env = string(locat) + "\n";
		write(fdmap[id], env.c_str(), env.length());
	}
}

/* 3 */
void S_env(vector<string> cmd, int id)
{
	setenv(cmd[1].c_str() , cmd[2].c_str(), 1);
	clientenv[id][cmd[1]] = cmd[2];
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

void SetClientEnv(int id)
{
	map<string,string> env=clientenv[id];
    for (map<string,string>::iterator it=env.begin(); it!=env.end(); ++it)
	{
        if(setenv((it->first).c_str(),(it->second).c_str(),1)<0)
		{
            cerr<<"SetAllEnvironment error : "<<strerror(errno)<<endl;
        }
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
int Outpipe(vector<int*>* pipe_vector, int outpipe, int cmdnum, int g, int id)
{
	int pipe_in_queue = cmd_queue[id].size();
	int loc_in_queue = -1;//PIPE到第幾個指令在queue的哪裡
	for(int a = 0; a < pipe_in_queue; a++)
	{
		if(cmd_queue[id][a] == outpipe)
		{
			loc_in_queue = a;
		}
	}
	return loc_in_queue;
}

int Midoutpipe(int g, int id)
{
	int pipe_in_queue = cmd_queue[id].size();
	int loc_in_queue = -1;//PIPE到第幾個指令在queue的哪裡
	for(int a = 0; a < pipe_in_queue; a++)
	{
		//cerr<<cmd_queue[a]<<endl;
		if(cmd_queue[id][a] == (g + 2))
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

void close_local_pipe(vector<int*>* pipe_vector, int pipe_num, int input_pipe, int out_in_pipe, int id)//把外面不要的pipe關了
{
	for(int j = 0 ; j < pipe_num ; j ++)
	{
		if(j != input_pipe && j != out_in_pipe)
		{
			close(pipe_vector[id][j][0]);
			close(pipe_vector[id][j][1]);
		}
	}
}

void CloseUserpipeLogout(int id)
{
	for(int i = 0; i < userpipe.size(); i ++)
	{
        if(sender[i] == id || receiver[i] == id)
		{
            close(userpipe[i][0]);
            close(userpipe[i][0]);
            delete[] userpipe[i];
            userpipe.erase(userpipe.begin() + i);
            sender.erase(sender.begin() + i);
            receiver.erase(receiver.begin() + i);
        }
    }
}

void CloseVectorPipeLogout(int id)
{
	for(int i = 0; i < cmd_queue[id].size(); i++)
	{
		close(pipe_vector[id][i][0]);
		close(pipe_vector[id][i][1]);
		delete[] pipe_vector[id][i];
		vector<int> cmdq = cmd_queue[id];
		cmdq.erase(cmdq.begin() + i);
		cmd_queue[id] = cmdq;
		vector<int*> pvector = pipe_vector[id];
		pvector.erase(pvector.begin() + i);
		pipe_vector[id] = pvector;
	}
}

/*  */
void ChildHandler(int signo)
{
    int status;
    while(waitpid(-1,&status,WNOHANG)>0){}
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
int SetInput(int nowcommand, int id)
{
	int pipe_in_queue = cmd_queue[id].size();
	int input_pipe = -1;
	for(int k = 0; k < pipe_in_queue; k ++)
	{
		int target = nowcommand + 1;
		if(cmd_queue[id][k] == target)
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
void SetInQueue(int next_loc, int cmdnum, int id)//把NPIPE放進pipe_vector,cmdnum為總指令數
{
	int queue_cmd = cmd_queue[id].size();
	int temp = 0;
	for(int a = 0 ; a < queue_cmd; a++)
	{
		//cerr<<"cmdqueue: "<<cmd_queue[a]<<endl;
		if(cmd_queue[id][a] == (next_loc + cmdnum))
		{
			temp = 1;
		}
	}
	if(temp == 0)
	{
		int* N_pipe = new int[2];
		pipe(N_pipe);
		pipe_vector[id].push_back(N_pipe);
		cmd_queue[id].push_back(next_loc + cmdnum);
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

int SetInputFromUpipe(int PipeFromUser,int id,string line)
{
	int getuser = -1;
    if(fdmap[PipeFromUser] < 0)
	{//user 不存在
        string errormsg = "*** Error: user #" + to_string(PipeFromUser) + " does not exist yet. ***\n";
		write(fdmap[id], errormsg.c_str(), errormsg.length());
        return -1;
    }
	else
	{
		for(int i = 0; i < sender.size();i++)
		{
			//如果有user pipe的存在，就把此index return
			for(int v = 0; v < sender.size(); v++)
			{
				if(sender[v] == PipeFromUser && receiver[v] == id)
				{
					getuser = v;
				}
			}
		}
		if(getuser == -1)
		{
			string errormsg = "*** Error: the pipe #" + to_string(PipeFromUser) + "->#" + to_string(id) + " does not exist yet. ***\n";
			write(fdmap[id], errormsg.c_str(), errormsg.length());
		}
		else
		{
			string recmsg = "*** "+ client_name[id] + " (#" + to_string(id) + ") just received from " + client_name[PipeFromUser];
			recmsg += " (#" + to_string(PipeFromUser) + ") by '"+ line + "' ***\n";
			Broadcast(recmsg);
		}

	}
    return getuser;
}

int SetOutputToUpipe(int id ,int PipeToUser,string line)
{
	int ero = 0;
	int getuser = -1;
    if(fdmap[PipeToUser] < 0)
	{
		string errormsg = "*** Error: user #" + to_string(PipeToUser) + " does not exist yet. ***\n";
		write(fdmap[id], errormsg.c_str(), errormsg.length());
		ero = 1;
	}
	else
	{
		for(int v = 0; v < sender.size(); v++)
		{
			if(sender[v] == id && receiver[v] == PipeToUser)
			{
				string errormsg = "*** Error: the pipe #" + to_string(id) + "->#" + to_string(PipeToUser) + " already exists. ***\n";
				write(fdmap[id], errormsg.c_str(), errormsg.length());
				ero = 1;
			}
		}
		if(ero == 0)
		{
			sender.push_back(id);
			receiver.push_back(PipeToUser);
			int* N_pipe = new int[2];
			pipe(N_pipe);
			userpipe.push_back(N_pipe);
			getuser = sender.size() -1;
			string semsg = "*** " + client_name[id] + " (#" + to_string(id) + ") just piped '" + line;
			semsg += "' to " + client_name[PipeToUser] + " (#" + to_string(PipeToUser) + ") ***\n";
			Broadcast(semsg);
		}
	}
	return getuser;
}

void CheckStderr(int **cmd_pipe, string check,int outpipe, int midout, int id)
{
	if(check[0] == '!')
	{
		if(midout >= 0)
		{
			dup2(pipe_vector[id][midout][1], STDERR);
		}
		else
		{
			dup2(cmd_pipe[outpipe][1], STDERR);
		}
	}
	else
	{
		dup2(fdmap[id], STDERR);
	}
}

/*  */
int number_pipe(vector<string> cmd, vector<int> pipe_locate, vector<string> recvec, vector<int> recloc, int file, int id, int PipeToUser, int PipeFromUser, int eatuserpipe, int outuserpipe)
{
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
	//cerr<<PipeToUser<<"  "<<PipeFromUser<<endl;
	if(PipeFromUser != -1)
	{
		PipeFromUser = SetInputFromUpipe(PipeFromUser, id, rec_cmdline);
	}
	if(PipeToUser != -1)
	{
		PipeToUser = SetOutputToUpipe(id, PipeToUser, rec_cmdline);
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
		P_env(cmd, id);
	}
	else if(cmd[0] == "setenv")
	{
		S_env(cmd, id);
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
			int input_pipe = SetInput(g, id);
			int outpipe = findNpipe(cmd, pipe_locate, g);
			int pipe_in_queue = cmd_queue[id].size();
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
				if(only_pipe == 1 && check[0] != '|' && check[0] != '!' && PipeToUser == -1)
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
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g, id);
							//cerr<<"C1_Input: "<<input_pipe<<"outpipe: "<<outpipe<<" pipe_vector:"<<pipe_vector.size()<<"out: "<<out<<endl;
							close_local_pipe(pipe_vector, pipe_in_queue, -1, out, id);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(pipe_vector[id][out][0]);
							close_userpipe(userpipe, PipeFromUser, -1);
							close(STDERR);

							if(check[0] == '!')
							{
								dup2(pipe_vector[id][out][1], STDERR);
							}
							else
							{
								dup2(fdmap[id], STDERR);
							}
							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(STDIN);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(STDIN);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							dup2(pipe_vector[id][out][1], STDOUT);
						}
						else
						{
							//cerr<<"C2_PipeFromUser: "<<PipeFromUser<<" userpipe:"<<userpipe.size()<<"outpipe: " <<outpipe<<endl;
							int midout = Midoutpipe(g, id);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, midout, id); //把其他PIPE關起來
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, outpipe);
							close_userpipe(userpipe, PipeFromUser, -1);
							if(g != outpipe)
							{
								close(cmd_pipe[g][0]);
								close(cmd_pipe[g][1]);
							}
							close(cmd_pipe[outpipe][0]);
							close(STDERR);

							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(STDIN);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(STDIN);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							if(midout >= 0)
							{
								close(cmd_pipe[outpipe][1]);
								close(pipe_vector[id][midout][0]);
								dup2(pipe_vector[id][midout][1], STDOUT);
							}
							else
							{
								dup2(cmd_pipe[outpipe][1], STDOUT);
							}
							CheckStderr(cmd_pipe, check, outpipe, midout, id);//檢查是否為'!'
						}
					}
					else if(only_pipe > 1 && input_pipe >= 0)//有先前的pipe而且有>一個的指令
					{
						if(outpipe > only_pipe -1)//|N, N>指令數
						{
							//cerr<<"C3\n";
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g, id);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out, id);
							close_userpipe(userpipe, -1, -1);
							close(STDOUT);
							close(STDIN);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(pipe_vector[id][input_pipe][1]);
							close(pipe_vector[id][out][0]);
							close(STDERR);

							if(check[0] == '!')
							{
								dup2(pipe_vector[id][out][1], STDERR);
							}
							else
							{
								dup2(fdmap[id], STDERR);
							}
							dup2(pipe_vector[id][input_pipe][0], STDIN);
							dup2(pipe_vector[id][out][1], STDOUT);
						}
						else
						{
							//cerr<<"C4: "<<input_pipe<<"outpipe: "<<outpipe<<" pipe_vector:"<<pipe_vector.size();
							int midout = Midoutpipe(g, id);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, midout, id); //把其他PIPE關起來
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, outpipe);
							close_userpipe(userpipe, -1, -1);
							close(STDOUT);
							close(STDIN);
							if(g != outpipe)
							{
								close(cmd_pipe[g][0]);
								close(cmd_pipe[g][1]);
							}
							close(pipe_vector[id][input_pipe][1]);
							close(cmd_pipe[outpipe][0]);
							close(STDERR);

							if(midout >= 0)
							{
								close(cmd_pipe[outpipe][1]);
								close(pipe_vector[id][midout][0]);
								dup2(pipe_vector[id][midout][1], STDOUT);
							}
							else
							{
								dup2(cmd_pipe[outpipe][1], STDOUT);
							}
							CheckStderr(cmd_pipe, check, outpipe, midout, id);//檢查是否為'!'
							dup2(pipe_vector[id][input_pipe][0], STDIN);
						}
					}
					else if(only_pipe == 1 && input_pipe < 0)//只有一個指令&沒有之前的PIPE
					{
						if(file == 1)//這條指令是寫檔
						{
							//cerr<<"C5_Input: "<<input_pipe<<" outpipe: "<<outpipe<<" pipe_vector: "<<pipe_vector.size()<<endl;
							close_local_pipe(pipe_vector, pipe_in_queue, -1, -1, id);
							close_userpipe(userpipe, PipeFromUser, -1);
							int file = open(cmd[cmd.size() - 2].c_str(), O_CREAT|O_RDWR|O_TRUNC,S_IREAD|S_IWRITE);
							close(STDOUT);
							close(STDERR);

							dup2(fdmap[id], STDERR);
							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(STDIN);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(STDIN);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							test_dup2(file, STDOUT);
							close(file);
						}
						else if(outpipe > only_pipe -1)
						{
							test_close(STDOUT);
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							close_userpipe(userpipe, PipeFromUser, -1);
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g, id);
							//cerr<<"C6_Input: "<<input_pipe<<" outpipe: "<<outpipe<<" pipe_vector: "<<pipe_vector.size()<<" out: "<<out<<endl;
							close_local_pipe(pipe_vector, pipe_in_queue, -1, out, id);
							test_close(pipe_vector[id][out][0]);
							close(STDERR);

							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(STDIN);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(STDIN);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							test_dup2(pipe_vector[id][out][1], STDOUT);
							if(check[0] == '!')
							{
								test_dup2(pipe_vector[id][out][1], STDERR);
							}
							else
							{
								dup2(fdmap[id], STDERR);
							}

						}
						else
						{
							//cerr<<outpipe<<"C7: "<<PipeFromUser<<" : "<<PipeToUser<<" pipe_vector: "<<userpipe.size()<<endl;
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							close_local_pipe(pipe_vector, pipe_in_queue, -1, -1, id);
							close_userpipe(userpipe, PipeFromUser, PipeToUser);
							close(STDOUT);
							close(STDERR);

							dup2(fdmap[id], STDERR);
							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(STDIN);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(STDIN);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							if(outuserpipe != -1)
							{
								if(PipeToUser != -1)
								{
									close(userpipe[PipeToUser][0]);
									dup2(userpipe[PipeToUser][1], STDOUT);
								}
								else
								{
									int nul = open("/dev/null", O_RDWR);
									dup2(nul, STDOUT);
								}
							}
							else
							{
								dup2(fdmap[id], STDOUT);
							}
						}
					}
					else if(only_pipe == 1 && input_pipe >= 0) //只有一個指令&有先前PIPE
					{
						if(file == 1)//這條指令是寫檔
						{
							//cerr<<"C8\n";
							int file = open(cmd[cmd.size() - 2].c_str(), O_CREAT|O_RDWR|O_TRUNC,S_IREAD|S_IWRITE);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1, id); //把其他PIPE關起來
							close_userpipe(userpipe, -1, -1);
							close(STDOUT);
							close(STDIN);
							close(pipe_vector[id][input_pipe][1]);
							close(STDERR);

							dup2(fdmap[id], STDERR);
							dup2(pipe_vector[id][input_pipe][0], STDIN);
							dup2(file, STDOUT);
							close(file);
						}
						else if(outpipe > g)
						{
							//cerr<<"C9\n";
							close(STDOUT);
							close(STDIN);
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g, id);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out, id);
							close_userpipe(userpipe, -1, -1);
							close(pipe_vector[id][input_pipe][1]);
							close(pipe_vector[id][out][0]);
							close(STDERR);

							dup2(fdmap[id], STDERR);
							dup2(pipe_vector[id][input_pipe][0], STDIN);
							dup2(pipe_vector[id][out][1], STDOUT);
						}
						else
						{
							//cerr<<"C10_input: "<<input_pipe<<"outpipe: "<<outpipe<<"cmd_queue:"<<cmd_queue.size()<<endl;
							close(STDIN);
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1, id);
							close_userpipe(userpipe, PipeToUser, -1);
							close(pipe_vector[id][input_pipe][1]);
							close(STDOUT);
							close(STDERR);

							dup2(fdmap[id], STDERR);
							dup2(pipe_vector[id][input_pipe][0], STDIN);
							if(outuserpipe != -1)
							{
								if(PipeToUser != -1)
								{
									close(userpipe[PipeToUser][0]);
									dup2(userpipe[PipeToUser][1], STDOUT);
								}
								else
								{
									int nul = open("/dev/null", O_RDWR);
									dup2(nul, STDOUT);
								}
							}
							else
							{
								dup2(fdmap[id], STDOUT);
							}
						}
					}
					char const *ex_line[1024];
					int k = 0 , i;
					for(i = 0 ; i < pipe_locate[0] ; i ++)
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

			}
			else
			{
				ForkNewProcess(pid);
				if(g == only_pipe - 1 && check[0] != '|' && check[0] != '!' && PipeToUser == -1)
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
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1, id);
							close_userpipe(userpipe, PipeFromUser, -1);
							int file = open(cmd[cmd.size() - 2].c_str(), O_CREAT|O_RDWR|O_TRUNC,S_IREAD|S_IWRITE);
							close(STDIN);
							close(STDOUT);
							close(cmd_pipe[g-1][1]);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(STDERR);

							if(input_pipe >= 0)
							{
								close(pipe_vector[id][input_pipe][1]);
								close(cmd_pipe[g-1][0]);
								dup2(pipe_vector[id][input_pipe][0] , STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							dup2(fdmap[id], STDERR);
							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(cmd_pipe[g-1][0]);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(cmd_pipe[g-1][0]);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							dup2(file, STDOUT);
							close(file);
						}
						else
						{
							//cerr<<"C12_INput: "<<PipeFromUser<<" PipeToUser: "<<PipeToUser<<" cmd_queue_size: "<<userpipe.size()<<endl;
							close(STDIN);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, -1, id);
							close_userpipe(userpipe, PipeFromUser, PipeToUser);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(cmd_pipe[g-1][1]);
							close(STDOUT);
							close(STDERR);

							dup2(fdmap[id], STDERR);
							if(input_pipe >= 0)
							{
								close(pipe_vector[id][input_pipe][1]);
								close(cmd_pipe[g-1][0]);
								dup2(pipe_vector[id][input_pipe][0] , STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(cmd_pipe[g-1][0]);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(cmd_pipe[g-1][0]);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							if(outuserpipe != -1)
							{
								if(PipeToUser != -1)
								{
									close(userpipe[PipeToUser][0]);
									dup2(userpipe[PipeToUser][1], STDOUT);
								}
								else
								{
									int nul = open("/dev/null", O_RDWR);
									dup2(nul, STDOUT);
								}
							}
							else
							{
								dup2(fdmap[id], STDOUT);
							}
						}
					}
					else if(g == only_pipe - 1 && check[0] == '|')//最後一個指令是'|N'
					{
						close(STDOUT);
						close(STDIN);
						int out = Outpipe(pipe_vector, outpipe, only_pipe, g, id);
						//cerr<<"C13_INput: "<<input_pipe<<" outpipe: "<<outpipe<<" cmd_queue_size: "<<cmd_queue.size()<<" out: "<<out<<endl;
						close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out, id);
						close_userpipe(userpipe, PipeFromUser, -1);
						close(cmd_pipe[g-1][1]);
						test_close(cmd_pipe[g][0]);
						test_close(cmd_pipe[g][1]);
						close(pipe_vector[id][out][0]);
						close(STDERR);

						if(input_pipe >= 0)
						{
							close(pipe_vector[id][input_pipe][1]);
							close(cmd_pipe[g-1][0]);
							dup2(pipe_vector[id][input_pipe][0] , STDIN);
						}
						else
						{
							dup2(cmd_pipe[g-1][0] , STDIN);
						}
						dup2(fdmap[id], STDERR);
						if(eatuserpipe == g && PipeFromUser != -1)
						{
							close(cmd_pipe[g-1][0]);
							close(userpipe[PipeFromUser][1]);
							dup2(userpipe[PipeFromUser][0], STDIN);
						}
						else if(eatuserpipe == g && PipeFromUser == -1)
						{
							close(cmd_pipe[g-1][0]);
							int nul = open("/dev/null", O_RDWR);
							dup2(nul, STDIN);
						}
						else
						{
							dup2(cmd_pipe[g-1][0] , STDIN);
						}
						if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
						{
							close(userpipe[PipeFromUser][0]);
							close(userpipe[PipeFromUser][1]);
						}
						dup2(pipe_vector[id][out][1], STDOUT);
					}
					else if(g == only_pipe - 1 && check[0] == '!') //最後一個指令是'!N'
					{
						close(STDOUT);
						close(STDIN);
						close(STDERR);
						int out = Outpipe(pipe_vector, outpipe, only_pipe, g, id);
						//cerr<<"C14_INput: "<<input_pipe<<" outpipe: "<<outpipe<<" cmd_queue_size: "<<cmd_queue.size()<<" out: "<<out<<endl;
						close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out, id);
						close_userpipe(userpipe, PipeFromUser, -1);
						close(cmd_pipe[g-1][1]);
						close(pipe_vector[id][out][0]);
						close(cmd_pipe[g][0]);
						close(cmd_pipe[g][1]);

						if(input_pipe >= 0)
						{
							close(pipe_vector[id][input_pipe][1]);
							close(cmd_pipe[g-1][0]);
							dup2(pipe_vector[id][input_pipe][0] , STDIN);
						}
						else
						{
							dup2(cmd_pipe[g-1][0] , STDIN);
						}
						if(eatuserpipe == g && PipeFromUser != -1)
						{
							close(cmd_pipe[g-1][0]);
							close(userpipe[PipeFromUser][1]);
							dup2(userpipe[PipeFromUser][0], STDIN);
						}
						else if(eatuserpipe == g && PipeFromUser == -1)
						{
							close(cmd_pipe[g-1][0]);
							int nul = open("/dev/null", O_RDWR);
							dup2(nul, STDIN);
						}
						else
						{
							dup2(cmd_pipe[g-1][0] , STDIN);
						}
						if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
						{
							close(userpipe[PipeFromUser][0]);
							close(userpipe[PipeFromUser][1]);
						}
						dup2(pipe_vector[id][out][1], STDERR);
						dup2(pipe_vector[id][out][1], STDOUT);
					}
					else //中間指令
					{
						if(outpipe > only_pipe -1)
						{
							//cerr<<"C15\n";
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, -1);
							int out = Outpipe(pipe_vector, outpipe, only_pipe, g, id);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, out, id);
							close_userpipe(userpipe, PipeFromUser, -1);
							close(STDOUT);
							close(STDIN);
							close(cmd_pipe[g][0]);
							close(cmd_pipe[g][1]);
							close(cmd_pipe[g-1][1]);
							close(pipe_vector[id][out][0]);
							close(STDERR);

							if(input_pipe >= 0)
							{
								close(pipe_vector[id][input_pipe][1]);
								close(cmd_pipe[g-1][0]);
								dup2(pipe_vector[id][input_pipe][0] , STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							if(check[0] == '!')
							{
								dup2(pipe_vector[id][out][1], STDERR);
							}
							else
							{
								dup2(fdmap[id], STDERR);
							}
							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(cmd_pipe[g-1][0]);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(cmd_pipe[g-1][0]);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							dup2(pipe_vector[id][out][1], STDOUT);
						}
						else
						{
							//cerr<<"C16: "<<input_pipe<<"outpipe: "<<outpipe<<"cmd_queue:"<<cmd_queue.size()<<endl;
							int midout = Midoutpipe(g, id);
							close_local_pipe(pipe_vector, pipe_in_queue, input_pipe, midout, id); //把其他PIPE關起來
							close_cmdpipe(cmd_pipe, cmd_next_pipe, g, outpipe);
							close_userpipe(userpipe, PipeFromUser, -1);
							close(STDOUT);
							close(STDIN);
							close(cmd_pipe[g-1][1]);
							close(cmd_pipe[outpipe][0]);
							close(STDERR);

							if(eatuserpipe == g && PipeFromUser != -1)
							{
								close(cmd_pipe[g-1][0]);
								close(userpipe[PipeFromUser][1]);
								dup2(userpipe[PipeFromUser][0], STDIN);
							}
							else if(eatuserpipe == g && PipeFromUser == -1)
							{
								close(cmd_pipe[g-1][0]);
								int nul = open("/dev/null", O_RDWR);
                                dup2(nul, STDIN);
							}
							else
							{
								dup2(cmd_pipe[g-1][0] , STDIN);
							}
							if(eatuserpipe != -1 && eatuserpipe != g && PipeFromUser != -1)
							{
								close(userpipe[PipeFromUser][0]);
								close(userpipe[PipeFromUser][1]);
							}
							if(midout >= 0)
							{
								close(cmd_pipe[outpipe][1]);
								close(pipe_vector[id][midout][0]);
								dup2(pipe_vector[id][midout][1], STDOUT);
							}
							else
							{
								dup2(cmd_pipe[outpipe][1], STDOUT);
							}
							CheckStderr(cmd_pipe, check, outpipe, midout, id);//檢查是否為'!'
						}
					}
					excfun(cmd, pipe_locate, pipe_num_temp);
				}
				pipe_num_temp ++;
			}
		}//for迴圈尾
	}
	if(only_pipe >= 2)
	{
		test_close(cmd_pipe[only_pipe-1][0]);
		test_close(cmd_pipe[only_pipe-1][1]);
		test_close(cmd_pipe[only_pipe-2][0]);
		test_close(cmd_pipe[only_pipe-2][1]);
		delete[] cmd_pipe[only_pipe-1];
		delete[] cmd_pipe[only_pipe-2];
	}

	for(int y = 0; y < cmd_queue[id].size(); y ++)
	{
		cmd_queue[id][y] -= only_pipe;
	}
	for(int r = 0 ; r < cmd_queue[id].size(); r++)
	{
		if(cmd_queue[id][r] <= 0)
		{
			close(pipe_vector[id][r][0]);
			close(pipe_vector[id][r][1]);
			delete[] pipe_vector[id][r];
			vector<int> cmdq = cmd_queue[id];
			cmdq.erase(cmdq.begin() + r);
			cmd_queue[id] = cmdq;
			vector<int*> pvector = pipe_vector[id];
			pvector.erase(pvector.begin() + r);
			pipe_vector[id] = pvector;
			//cmd_queue.erase(cmd_queue[id].begin() + r);
		}
	}
	if(PipeFromUser != -1)
	{
		close(userpipe[PipeFromUser][0]);
		close(userpipe[PipeFromUser][1]);
		delete [] userpipe[PipeFromUser];
		userpipe.erase(userpipe.begin() + PipeFromUser);
		sender.erase(sender.begin() + PipeFromUser);
		receiver.erase(receiver.begin() + PipeFromUser);
	}

	while(!(child_pid.empty()))
	{
		int status;
		waitpid(child_pid.front(),&status,0);
		child_pid.pop();
	}
}
/*  */
int exe_cmd(vector<string> cmd, vector<int> pipe_locate, vector<string> recvec, vector<int> recloc, int clientfd, int PipeToUser, int PipeFromUser, int eatuserpipe, int outuserpipe)
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
	number_pipe(cmd, pipe_locate, recvec, recloc, fi, clientfd, PipeToUser, PipeFromUser, eatuserpipe, outuserpipe);
	return 1;
}


/*  */
int cmp_cmdline(string arg, int id) // separate string
{
	vector<int> pipe_locate;
    int start = 0, have_cmd = 0;
	int PipeToUser = -1, PipeFromUser = -1;
	int eatuserpipe = -1;
	int outuserpipe = -1;
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
					outuserpipe = pipe_locate.size();
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
					outuserpipe = pipe_locate.size();
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
				SetInQueue(next_loc, cmdnum, id);
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
		exe_cmd(cmd, pipe_locate,recvec,recloc, id, PipeToUser, PipeFromUser, eatuserpipe, outuserpipe);
	}
	return 1;
}

int main(int argc, char **argv)
{
	int serverfd, clientfd;
	fd_set read_fds;
    struct sockaddr_in  server, client;
    char buff[MAXLINE];
    int  n;
	int fdmax = 0;
	int yes=1; // 供底下的 setsockopt() 設定 SO_REUSEADDR
	socklen_t clientSize = sizeof(client);
	FD_ZERO(&master); // 清除 master 與 temp sets
  	FD_ZERO(&read_fds);
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(atoi(argv[1]));
	if( (serverfd = socket(AF_INET, SOCK_STREAM, 0)) == -1 )
    {
        printf("create socket error: %s(errno: %d)\n",strerror(errno),errno);
        return 0;
    }
	for(int i=1;i<=clientmax;i++)
	{
        fdmap[i]=-1;//設定fdmap的初始值
    }
	fdmap[0] = serverfd;
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
	FD_SET(serverfd, &master); // 將 listener 新增到 master set
	fdmax = serverfd;
	while(1)
    {
		read_fds = master;
		if(select(fdmax+1, &read_fds, NULL, NULL, NULL) < 0)
		{
			cerr<<"select error\n";
		}
		else
		{
			for(int i = 0; i <= clientmax; i++) 
			{
				if(fdmap[i] > 0)
				{
					if (FD_ISSET(fdmap[i], &read_fds))// 我們找到一個！！
					{ 
						if (i == 0)
						{
							// handle new connections
							clientfd = accept(serverfd,(struct sockaddr *)&client,&clientSize);
							if (clientfd == -1) 
							{
								cerr<<"accept";
							}
							else
							{
								FD_SET(clientfd, &master);//新增到 master set
                                if (clientfd > fdmax) {// 持續追蹤最大的 fd
                                    fdmax = clientfd;
                                }
                                for(int j = 1; j <= clientmax; j++)
								{
                                    if(fdmap[j]==-1)
									{
                                        fdmap[j]=clientfd;
                                        client_name[j]="(no name)";
                                        clientenv[j]["PATH"]="bin:.";
                                        write(fdmap[j],welcome.c_str(),welcome.length());
										string cip = inet_ntoa(client.sin_addr);
										clientIP[j] = cip;
										clientPORT[j] = ntohs(client.sin_port);
                                        string msg="*** User '"+client_name[j]+ "' entered from " + clientIP[j] + ":" + to_string(clientPORT[j]) + ". ***\n";
                                        Broadcast(msg);
                                        write(fdmap[j], pa.c_str(), pa.length());
                                        break;
                                    }
                                }
							}
						} 
						else 
						{
							// 處理來自 client 的資料
							ClearAllEnvironment();
							SetClientEnv(i);
							SetBuildin();
							string line;
							int state = 1;
							memset (&buff, 0, MAXLINE);
							do
							{
								n = read(fdmap[i], buff, sizeof(buff));
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
								string exitmsg = "*** User '" + client_name[i] + "' left. ***\n";
								Broadcast(exitmsg);
								FD_CLR(fdmap[i], &master);
								close(fdmap[i]);
								fdmap[i] = -1;
								client_name[i] = "";
								clientIP[i] = "";
								clientPORT[i] = -1;
								clientenv.erase(clientenv.find(i));//移除此user的環境變數
								CloseUserpipeLogout(i);
								CloseVectorPipeLogout(i);
							}
							else if(line != "")
							{
								state = cmp_cmdline(line, i);
								write(fdmap[i], pa.c_str(), pa.length());
							}
							else if(line == "")
							{
								write(fdmap[i], pa.c_str(), pa.length());
							}
						} // END handle data from client
					} // END got new incoming connection
				}
			} // END looping through file descriptors
		}
    }
	return 0;
}