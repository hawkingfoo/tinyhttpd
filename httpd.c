/* *********************************************
 *               源码阅读顺序
 * main-> startup-> accept_request-> execute_cgi
 * *********************************************/

#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"

/* ***************************
 *          函数声明
 * ***************************/

/* 处理从套接字上监听到的一个HTTP请求 */
void accept_request(int);
/* 返回给客户端这是个错误请求，HTTP状态码400 BAD REQUEST */
void bad_request(int);
/* 读取服务器上某个文件写到 socket 套接字 */
void cat(int, FILE *);
/* 主要处理发生在执行cgi程序时出现的错误 */
void cannot_execute(int);
/* 将错误信息写到perror并退出 */
void error_die(const char *);
/* 运行cgi程序的处理 */
void execute_cgi(int, const char *, const char *, const char *);
/* 读取套接字的一行，把回车换行等统一为换行符结束 */
int get_line(int, char *, int);
/* 把HTTP响应头写到套接字 */
void headers(int, const char *);
/* 处理找不到请求文件的情况 */
void not_found(int);
/* 调用cat把服务器文件返回给浏览器 */
void serve_file(int, const char *);
/* 初始化httpd服务，包括建立socket等过程 */
int startup(u_short *);
/* 返回给浏览器表明收到的HTTP请求所用的method不被支持*/
void unimplemented(int);


/* ********************************
 * @描述：处理客户端HTTP请求
 * @输入：[in] client:	客户端地址
 * @输出： 无
 * ********************************/
void accept_request(int client)
{
	char buf[1024];
	int numchars;
	char method[255];
	char url[255];
	char path[512];
	size_t i, j;	// unsigned int
	struct stat st; // 保存文件信息
	int cgi = 0;	// 当服务端认为它是一个CGI时变成1
	char *query_string = NULL;

	/* 得到请求的第一行 */
	numchars = get_line(client, buf, sizeof(buf));
	i = 0;
	j = 0;
	
	/* 把客户端的请求方法存到method数组,遇到空格则停止 */
	while(!ISspace(buf[j]) && (i < sizeof(method) - 1))
	{
		method[i] = buf[j];
		i++; j++;
	}
	method[i] = '\0';
	
	/* 既不是GET也不是POST的情况,忽略大小写进行比较 */
	if(strcasecmp(method, "GET") && strcasecmp(method, "POST"))
	{
		unimplemented(client);
		return;
	}
	
	/* POST的情况，开启CGI */
	if(strcasecmp(method, "POST") == 0)
		cgi = 1;
	
	/* 读取URL地址 */
	i = 0;
	while(ISspace(buf[j]) && (j < sizeof(buf)))// 过滤掉空格
		j++;
	while(!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < sizeof(buf)))
	{
		url[i] = buf[j]; // 存下URL
		i++; j++;
	}
	url[i] = '\0';

	/* 处理GET方法 */
	if(strcasecmp(method, "GET") == 0)
	{
		query_string = url;
		/* 找到URL中的? */
		while((*query_string != '?') && (*query_string != '\0'))
			query_string++;
		/* GET方法特点，?后面为参数 */
		if(*query_string == '?')
		{
			cgi = 1;
			*query_string = '\0';
			query_string++;		//query_string指向'?'后面
		}
	}

	/* 格式化URL到path数组，html文件在htdocs目录中 */
	sprintf(path, "htdocs%s", url);
	/* 默认情况为index.html */
	if(path[strlen(path) - 1] == '/')// path中最后一个字符
		strcat(path, "index.html");

	/* 根据路径找到对应文件 */
	if(stat(path, &st) == -1) // 通过文件名path获取文件信息，并保存到st中,-1表示失败
	{
		/* 读取并丢弃header */
		while((numchars > 0) && strcmp("\n", buf))// strcmp 相等返回0
			numchars = get_line(client, buf, sizeof(buf));
		not_found(client);
	}
	else
	{
		/* 如果是目录，则默认使用该目录下的index.html 文件*/
		if((st.st_mode & S_IFMT) == S_IFDIR)
			strcat(path, "/index.html");
		if((st.st_mode & S_IXUSR) ||
		   (st.st_mode & S_IXGRP) ||
		   (st.st_mode & S_IXOTH)    )
			cgi = 1;

		if(!cgi) // cgi == 0
			serve_file(client, path);
		else	 // cgi == 1
			execute_cgi(client, path, method, query_string);
	}
	/* 断开与客户端的连接 */
	close(client);
}

/* *************************************
 * @描述：返回错误请求给客户端
 * @输入：[in] client: 客户端文件描述符
 * @输出：无
 * *************************************/
void bad_request(int client)
{
	char buf[1024];
	
	/* 回应客户端错误的HTTP请求 */
	sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
	send(client, buf, strlen(buf), 0);
	
	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "<You browser sent a bad request, >");
	send(client, buf, strlen(buf), 0);
	sprintf(buf, "such as a POST without a Content-Length.\r\n");
	send(client, buf, strlen(buf), 0);
}

/* *************************************
 * @描述：将服务器上某个文件写入socket
 * @输入：[in] client:	客户端文件描述符
 * @	  [in] resource:文件
 * @输出：无
 * *************************************/
void cat(int client, FILE *resource)
{
	char buf[1024];
	
	/* 读取文件中的数据到socket */
	fgets(buf, sizeof(buf), resource);
	while(!feof(resource))
	{
		send(client, buf, strlen(buf), 0);
		fgets(buf, sizeof(buf), resource);
	}
}

/* *************************************
 * @描述：通知客户端，服务端CGI无法执行
 * @输入：[in] client: 客户端文件描述符
 * @输出：无
 * *************************************/
void cannot_execute(int client)
{
	char buf[1024];
	
	/* 回应客户端cgi无法执行 */
	sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
	send(client, buf, strlen(buf), 0);
}

/* *****************************
 * 将错误信息写到perror，并退出
 * *****************************/
void error_die(const char *sc)
{
	/* 出错信息处理*/
	perror(sc);
	exit(1);
}

/* ***********************************************
 * @描述：执行CGI程序
 * @输入：[in] client: 客户端文件描述符
 * 	  [in] path:   路径
 * 	  [in] method: 请求方法
 * 	  [in] query_string:参数
 * @输出：无
 * ***********************************************/
void execute_cgi(int client, const char *path, const char *method, const char *query_string)
{
	char buf[1024];
	int cgi_output[2];
	int cgi_input[2];
	pid_t pid;
	int status;
	int i;
	char c;
	int numchars = 1;
	int content_length = -1;
	
	buf[0] = 'A';	buf[1] = '\0';
	/* GET请求，读取并丢弃header */
	if(strcasecmp(method, "GET") == 0)
		while((numchars > 0) && strcmp("\n", buf))
			numchars = get_line(client, buf, sizeof(buf));
	else /* POST */
	{
		/* 对POST的HTTP请求中找出content_length */
		numchars = get_line(client, buf, sizeof(buf));
		while((numchars > 0) && strcmp("\n", buf))
		{
			/* 利用\0进行分割 */
			buf[15] = '\0'; // 15指向冒号后的空格,Content-Length: 123
			/* HTTP 请求的特点 */
			if(strcasecmp(buf, "Content-Length:") == 0)
				content_length = atoi(&buf[16]);
			numchars = get_line(client, buf, sizeof(buf));
		}
		/* 没有找到content_length */
		if(content_length == -1)
		{
			bad_request(client); /* 错误的请求 */
			return;
		}
	}
	
	/* 正确的请求，HTTP状态码200 */
	sprintf(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);

	/* 建立管道 */
	if(pipe(cgi_output) < 0)
	{
		cannot_execute(client);	// 错误处理
		return;
	}
	if(pipe(cgi_input) < 0)
	{
		cannot_execute(client);	// 错误处理
		return;
	}

	if((pid = fork()) < 0)
	{
		cannot_execute(client);	// 错误处理
		return;
	}
	if(pid == 0)	/* 子进程，cgi script */
	{
		char meth_env[255];	// 环境变量
		char query_env[255];	// 环境变量
		char length_env[255];	// 环境变量
		
		/* 把STDOUT重定向到cgi_output的写入端*/
		dup2(cgi_output[1], 1);
		/* 把STDIN重定向到cgi_input的读取端*/
		dup2(cgi_input[0], 0);
		/* 关闭cgi_output的读取端和cgi_input的写入端*/
		close(cgi_output[0]);
		close(cgi_input[1]);
		/* 设置request_method的环境变量 */
		sprintf(meth_env, "REQUEST_METHOD=%s", method);
		putenv(meth_env);

		if(strcasecmp(method, "GET") == 0)
		{
			/* 设置query_string的环境变量 */
			sprintf(query_env, "QUERY_STRING=%s", query_string);
			putenv(query_env);
		}
		else /* POST */
		{
			/* 设置content_length的环境变量 */
			sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
			putenv(length_env);
		}
		/* 用execl运行cgi程序 */
		execl(path, path, NULL);
		exit(0);
	}
	else		/* 父进程 */
	{
		/* 关闭cgi_output的写入端和cgi_input的读取端 */
		close(cgi_output[1]);
		close(cgi_input[0]);
		if(strcasecmp(method, "POST") == 0)
		{
			/* 接收POST过来的数据 */
			for(i = 0; i < content_length; i++)
			{
				recv(client, &c, 1, 0);
				/* 把POST数据写入cgi_input，现在重定向到STDIN */
				write(cgi_input[1], &c, 1);
			}
		}
		/* 读取cgi_output的管道输出到客户端，该管道输入时STDOUT */
		while(read(cgi_output[0], &c, 1) > 0)
			send(client, &c, 1, 0);

		/* 关闭管道 */
		close(cgi_output[0]);
		close(cgi_input[1]);
		/* 等待子进程 */
		waitpid(pid, &status, 0); // 防止子进程退出变成僵尸进程
	}
}

/* *******************************************
 * @描述：
 * @输入：[in] sock: 客户端文件描述符
 * 	  [in] buf:  用来存放一行数据的buffer
 * 	  [in] size: buffer的大小
 * @输出：返回buffer存放数据的大小
 * *******************************************/
int get_line(int sock, char *buf, int size)
{
	int i = 0;
	char c = '\0';
	int n;
	
	while((i < size - 1) && (c != '\n'))
	{
		/* 一次从sock接收一个字节 */
		n = recv(sock, &c, 1, 0);
		if(n > 0)
		{
			if(c == '\r')
			{
				/* 使用MSG_PEEK标志使下一次读取依然可以得到这次
				 * 读取的内容，可以认为接收窗口不滑动 */
				n = recv(sock, &c, 1, MSG_PEEK);
				/* 如果是换行符则吸收掉*/
				if((n > 0) && (c == '\n'))
					recv(sock, &c, 1, 0);
				else
					c = '\n';	// "\r\n"结束，退出循环
			}
			/* 存到缓冲区 */
			buf[i] = c;
			i++;
		}
		else
			c = '\n';
	}
	buf[i] = '\0';
	/* 返回buf数组的大小 */
	return(i);
}

/* **************************************
 * @描述：返回关于文件的响应信息
 * @输入：[in] client:	客户端文件描述符
 * @	  [in] filename:文件名
 * @输出：无
 * *************************************/
void headers(int client, const char *filename)
{
	char buf[1024];
	(void)filename;
	
	/* 正常的HTTP header */
	strcpy(buf, "HTTP/1.0 200 OK\r\n");
	send(client, buf, strlen(buf), 0);
	
	strcpy(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);

	strcpy(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
}

/* ****************************************
 * @描述：找不到请求文件，返回404给客户端
 * @输入：[in] client: 客户端文件描述符
 * @输出：无
 * ****************************************/
void not_found(int client)
{
	char buf[1024];
	
	sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);
	
	sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "your request because the resource specified\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "is unavailable or nonexistent.\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

/* ***************************************
 * @描述：发送一个文件到客户端
 * @输入：[in] client:	客户端文件描述符
 * 	  [in] filename:文件名
 * @输出：无
 * **************************************/
void serve_file(int client, const char *filename)
{
	FILE *resource = NULL;
	int numchars = 1;
	char buf[1024];

	/* 读取并丢弃header */
	buf[0] = 'A';
	buf[1] = '\0';
	while((numchars > 0) && strcmp("\n", buf))
		numchars = get_line(client, buf, sizeof(buf));
	
	/* 打开server 文件 */
	resource = fopen(filename, "r");
	if(resource == NULL)
		not_found(client);
	else
	{
		/* 写HTTP header */
		headers(client, filename);
		/* 复制文件 */
		cat(client, resource);
	}
	fclose(resource);
}

/* ****************************************************
 * @描述：初始化httpd，如果端口为空，则动态分配一个端口
 * @输入：[in] port:	端口号
 * @输出：[out] 返回socket id
 * ****************************************************/
int startup(u_short *port)
{
	int httpd = 0;		// 服务端文件描述符，和server_sock一样
	struct sockaddr_in name;// 服务端地址
	
	/* 建立socket */
	httpd = socket(PF_INET, SOCK_STREAM, 0); // TCP
	if(httpd == -1)
		error_die("socket");

	memset(&name, 0, sizeof(name));
	name.sin_family = AF_INET;
	name.sin_port = htons(*port); // PORT
	name.sin_addr.s_addr = htonl(INADDR_ANY);

	/* Bind */
	if(bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)
		error_die("bind");

	/* 如果当前指定端口为0，getsockname则动态随机分配一个端口 */
	if(*port == 0)
	{
		socklen_t namelen = sizeof(name);
		if(getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)
			error_die("getsockname");
		*port = ntohs(name.sin_port);
	}
	
	/* Listen */
	if(listen(httpd, 5) < 0)
		error_die("listen");
	/* 返回socket id */
	return(httpd);
}

/* ***************************************
 * @描述：通知客户端其请求的方法不被支持
 * @输入：[in] client: 客户端文件描述符
 * @输出：无
 * ***************************************/
void unimplemented(int client)
{
	char buf[1024];
	sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, SERVER_STRING);
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "Content-Type: text/html\r\n");
	send(client, buf, strlen(buf), 0);
	
	sprintf(buf, "\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "</TITLE></HEAD>\r\n");
	send(client, buf, strlen(buf), 0);
	
	sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	send(client, buf, strlen(buf), 0);

	sprintf(buf, "</BODY></HTML>\r\n");
	send(client, buf, strlen(buf), 0);
}

int main()
{
	int server_sock = -1;		// 服务端文件描述符
	u_short port = 8888;		// 服务端绑定的端口
	int client_sock = -1;		// 客户端文件描述符
	struct sockaddr_in client_name;	// 客户端地址
	socklen_t client_name_len = sizeof(client_name);
	pthread_t newthread;

	/* 在对应端口建立 httpd 服务 */
	server_sock = startup(&port);
	printf("httpd running on port %d\n", port);

	while(1)
	{
		/* 套接字收到客户端连接请求 */
		client_sock = accept(server_sock,
				     (struct sockaddr *)&client_name,
				     &client_name_len);
		if(client_sock == -1)
			error_die("accpet");
		/* accept_request(client_sock); */
		if(pthread_create(&newthread, NULL, accept_request, client_sock) != 0)
			perror("pthread_create");
	}
	close(server_sock);
	return(0);
}

