/*

Source Server Remote Console v1.0.5
by lxndr (lxndr87i@gmail.com)
GPLv3

Home page: https://github.com/lxndr/srcon

*/


#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <readline/readline.h>
#include <readline/history.h>


#define RCON_OUT_AUTH		3
#define RCON_OUT_EXEC		2
#define RCON_IN_AUTH		2
#define RCON_IN_RESPONSE	0


static char prompt[256];
static int running = 1;
static int interactive = 0;
static int quiet = 0;
static int sock = 0;
static char *command = NULL;


static void
rl_clean ()
{
	rl_set_prompt ("");
	rl_replace_line ("", 0);
	rl_redisplay ();
}


static void
rl_print (const char *fmt, ...)
{
	int point = rl_point;
	char *line = rl_copy_text (0, rl_end);
	rl_clean ();
	
	va_list va;
	va_start (va, fmt);
	vprintf (fmt, va);
	va_end (va);
	
	rl_set_prompt (prompt);
	rl_replace_line (line, 0);
	rl_point = point;
	rl_redisplay ();
	
	free (line);
}


static void
quiet_print (const char *fmt, ...)
{
	if (quiet)
		return;
	
	va_list va;
	va_start (va, fmt);
	vprintf (fmt, va);
	va_end (va);
}


static void
parse_address (const char *addr, char *host, int maxhost, uint16_t *port)
{
	int len;
	char *f = strchr (addr, ':');
	len = f ? f - addr : strlen (addr);
	if (len > maxhost - 1)
		len = maxhost - 1;
	strncpy (host, addr, len);
	host[len] = 0;
	*port = f ? atoi (f + 1) : 27015;
}


static int
establish_connection (const char *host, uint16_t port)
{
	quiet_print ("Connecting to %s:%d... ", host, port);
	fflush (stdout);
	
	struct hostent *hp = gethostbyname (host);
	if (!hp) {
		quiet_print ("FAILED!\n");
		quiet_print ("Unknown address %s:%d\n", host, port);
		return 0;
	}
	
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons (port);
	addr.sin_addr = * (struct in_addr *) hp->h_addr_list[0];
	
	sock = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	
	if (connect (sock, (struct sockaddr *) &addr, sizeof (addr)) != 0) {
		quiet_print ("FAILED!\n");
		quiet_print ("Could not connect to the server: %s\n", strerror (errno));
		return 0;
	}
	
	quiet_print ("done.\n");
	return 1;
}


static int
send_packet (int type, const char *cmd)
{
	size_t len = strlen (cmd);
	int size = len + 10;
	
	/* form packet data */
	char *data = malloc (size + 4);
	* (int *) (data + 0) = size;
	* (int *) (data + 4) = 0;
	* (int *) (data + 8) = type;
	memcpy (data + 12, cmd, len);
	* (short *) (data + size + 2) = 0;
	
	int ret = send (sock, data, size + 4, 0);
	if (ret == 0) {
		quiet_print ("Connection closed\n");
		running = 0;
	} else if (ret == -1) {
		quiet_print ("Error while sending: %s\n", strerror (errno));
		running = 0;
	}
	
	free (data);
	return ret;
}


static int
recv_packet (int *id, int *type, char *text)
{
	int size, ret;
	
	ret = recv (sock, &size, 4, MSG_WAITALL);
	if (ret <= 0)
		return ret;
	
	char *data = malloc (size);
	ret = recv (sock, data, size, MSG_WAITALL);
	if (ret <= 0)
		return ret;
	
	*id = * (int *) data;
	*type = * (int *) (data + 4);
	memcpy (text, data + 8, size - 9); /* this also copies terminating null */
	
	free (data);
	return ret;
}


static void
on_authentication ()
{
	/* send commands specified on command line with (-c option) */
	if (command) {
		quiet_print ("Sending commands from option...\n");
		send_packet (RCON_OUT_EXEC, command);
		command = NULL;
	}
	
	/* in interactive mode, readline takes care about stdin */
	if (!interactive) {
		if (!isatty (fileno(stdin))) { /* pipe is used */
			quiet_print ("Sending commands from stdin...\n");
			
			char line[1024];
			while (!feof (stdin) && fgets (line, 1024, stdin)) {
				/* PARANOID: fgets also gets a \n. a few tests showed that
					servers don't care about it. but just in case, let's remove it*/
				/* TODO: it would be nice to pack all commands in one or more packets */
				size_t last = strlen (line) - 1;
				if (line[last] == '\n')
					line[last] = 0;
				send_packet (RCON_OUT_EXEC, line);
			}
		}
		
		/* currently in query mode we don't receive respond
			thus no need to continue */
		running = 0;
	}
}


static void
process_response ()
{
	int ret, id, type;
	char text[4096];
	
	ret = recv_packet (&id, &type, text);
	if (ret == 0) {
		rl_print ("Connection closed\n");
		running = 0;
		return;
	} else if (ret == -1) {
		rl_print ("Error while recieveing: %s\n", strerror (errno));
		running = 0;
		return;
	}
	
	if (type == RCON_IN_AUTH) {
		if (id == -1) {
			rl_print ("Authentication attempt failed\n");
			running = 0;
		} else {
			rl_print ("Successfully authenticated.\n");
			on_authentication ();
		}
	} else if (type == RCON_IN_RESPONSE) {
		if (*text)
			rl_print (text);
	}
}


static void
handle_line (char* line)
{
	/* FIXME: line == NULL means the end of stdin stream */
	if (!line) {
		running = 0;
		return;
	}
	
	if (*line) {
		if (strcmp (line, "logout") == 0) {
			running = 0;
/* disabled it. had a few troubles
		} else if (strcmp (line, "exit") == 0 || strcmp (line, "quit") == 0) {
			char *answer = readline ("This command will shut down the server. Are you sure you want it? [yes/NO]: ");
			if (strcmp (answer, "yes") == 0)
				send_packet (RCON_OUT_EXEC, line);
			else if (*answer == 'y')
				printf ("Why 'why'? This is a serious matter. Please type 'yes' if you are sure.\n");
			free (answer);
*/
		} else {
			add_history (line);
			send_packet (RCON_OUT_EXEC, line);
		}
	}
	
	free (line);
}


static void
interactive_mode (const char *host, uint16_t port)
{
	char *history_file = NULL;
	
	/* form prompt */
	sprintf (prompt, "\033[0;31mrcon@\033[0m%s:%d \033[0;31m>\033[0m ",
		host, port);
	
	/* history file path */
	char *homedir = getenv ("HOME");
	if (homedir && *homedir) {
		const char *fname = "/.srcon_history";
		size_t len = strlen (homedir);
		history_file = malloc (len + strlen (fname) + 1);
		memcpy (history_file, homedir, len + 1);
		strcat (history_file, fname);
	}
	
	/* initialize readline */
	using_history ();
	if (history_file)
		read_history (history_file);
	rl_callback_handler_install (prompt, handle_line);
	
	struct pollfd fds[2] = {
		{STDIN_FILENO,	POLLIN,	0},
		{sock,		POLLIN,	0}
	};
	
	while (running) {
		int ret = poll (fds, 2, -1);
		if (ret > 0) {
			if (fds[1].revents & POLLIN)
				process_response ();
			
			if (fds[1].revents & POLLERR) {
				rl_print ("POLLERR: An error has occured on the device or stream.\n");
				running = 0;
			}
			
			if (fds[1].revents & POLLHUP) {
				rl_print ("POLLHUP: The device has been disconnected.\n");
				running = 0;
			}
			
			if (fds[0].revents & POLLIN)
				rl_callback_read_char ();
			else if (fds[0].revents & POLLHUP)
				running = 0;
		} else if (ret == -1) {
			if (ret != EINTR)
				printf ("A poll error has accured: %s\n", strerror (errno));
		}
	}
	
	rl_clean ();
	rl_callback_handler_remove ();
	if (history_file) {
		write_history (history_file);
		free (history_file);
	}
}


static void
query_mode ()
{
	struct pollfd fd = {sock, POLLIN, 0};
	
	while (running) {
		int ret = poll (&fd, 1, -1);
		if (ret > 0) {
			if (fd.revents & POLLIN)
				process_response ();
			
			if (fd.revents & POLLERR) {
				quiet_print ("POLLERR: An error has occured on the device or stream.\n");
				running = 0;
			}
			
			if (fd.revents & POLLHUP) {
				quiet_print ("POLLHUP: The device has been disconnected.\n");
				running = 0;
			}
		} else if (ret == -1) {
			if (ret != EINTR)
				quiet_print ("A poll error has accured: %s\n", strerror (errno));
		}
	}	
}


static void
print_help ()
{
	puts ("Usage: srcon [OPTIONS] HOST[:PORT]\n"
		"Connect to a Valve Source Server via RCon protocol.\n\n"
		"Options:\n"
		"  -p PASSWORD   rcon password\n"
		"  -h            show this help message and exit\n"
		"  -v            show version information and exit\n"
		"  -i            interactive shell mode\n"
		"  -c            command(s) to send on startup\n"
		"  -q            only show response");
}


static void
print_version ()
{
	puts ("Source Server Remote Console v1.0.5\n"
		"by Alexander AB (lxndr87i@gmail.com)");
}


int
main (int argc, char **argv)
{
	int opt;
	char *address, *password = NULL;
	
	/* command line setting */
	while ((opt = getopt (argc, argv, "hvp:c:iq")) != -1) {
		switch (opt) {
		case 'h':
			print_help ();
			return EXIT_SUCCESS;
		case 'v':
			print_version ();
			return EXIT_SUCCESS;
		case 'p':
			password = optarg;
			break;
		case 'c':
			command = optarg;
			break;
		case 'i':
			interactive = 1;
			break;
		case 'q':
			quiet = 1;
			break;
		default:
			print_help ();
			return EXIT_FAILURE;
		}
	}
	
	address = argv[optind];
	if (!address) {
		print_help ();
		return EXIT_SUCCESS;
	}
	
	/* connection */
	char host[128];
	uint16_t port;
	parse_address (address, host, 128, &port);
	if (!establish_connection (host, port))
		return EXIT_FAILURE;
	
	/* password packet */
	send_packet (RCON_OUT_AUTH, password);
	
	if (interactive)
		interactive_mode (host, port);
	else
		query_mode ();
	
	quiet_print ("Disconnecting... ");
	fflush (stdout);
	close (sock);
	quiet_print ("done.\n");
	
	return EXIT_SUCCESS;
}
