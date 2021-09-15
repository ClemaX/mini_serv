#include <sys/socket.h>
#include <sys/select.h>

#include <arpa/inet.h>

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>

typedef struct
{
	size_t	size;
	size_t	len;
	char	*data;
}	buffer;


typedef struct
{
	buffer	r;
	buffer	w;
}	client;

static int	invalid_arguments()
{
	write(STDERR_FILENO, "Wrong number of arguments\n", 26);
	return 1;
}

static int	fatal_error()
{
	write(STDERR_FILENO, "Fatal error\n", 12);
	return 1;
}

static void	ft_memmove(char *dst, const char *src, size_t n)
{
	if (dst != src)
	{
		if (dst < src || (size_t)(dst - src) > n)
			while (n--)
				*dst++ = *src++;
		else
			while (n--)
				dst[n] = src[n];
	}
}

static int	buff_resize(buffer *buff, size_t new_size)
{
	char	*new_data;
	int		err;

	if (new_size != buff->size)
	{
		printf("Reallocating len: %zu, size %zu to %zu", buff->len,
			buff->size, new_size);
		if (new_size < buff->size)
			buff->len = new_size;
		new_data = realloc(buff->data, new_size);
		err = -(new_data == NULL);
		if (err == 0)
			buff->size = new_size;
		else
		{
			free(buff->data);
			buff->len = 0;
			buff->size = 0;
		}
		buff->data = new_data;
	}
	else
		err = 0;
	return err;
}

static void	buff_clr(buffer *buff)
{
	free(buff->data);
	bzero(buff, sizeof(*buff));
}

static int	buff_read(buffer *buff, int fd)
{
	int ret = recv(fd, buff->data + buff->len, buff->size - buff->len, 0);

	if (ret > 0)
	{
		buff->len += ret;
		buff->data[buff->len] = '\0';
	}
	return ret;
}

static int	buff_write(buffer *buff, int fd)
{
	int	ret = send(fd, buff->data, buff->len, 0);

	if (ret > 0)
	{
		buff->len -= ret;
		ft_memmove(buff->data, buff->data + ret, buff->len);
		ret = 1;
	}
	return ret;
}

static int  socket_new()
{ return socket(PF_INET, SOCK_STREAM, IPPROTO_IP); }

static int  socket_bind(int socket_fd, int port)
{
	const struct sockaddr_in	addr
		= {0, AF_INET, htons(port), {htonl(INADDR_LOOPBACK)}, {0}};

	return bind(socket_fd, (const struct sockaddr*)&addr, sizeof(addr));
}

static int	cli_io(client *client, int fd, bool readable, bool writable)
{
	int		ret;
	char	*msg_end;

	ret = 1;

	if (writable)
		ret = buff_write(&client->w, fd);
	if (ret > 0 && readable)
	{
		ret = buff_read(&client->r, fd);
		if (ret > 0)
		{
			msg_end = strstr(client->r.data + client->r.len - ret, "\n");
			if (msg_end != NULL)
				ret = msg_end - client->r.data + 1; // returning 1 here is ambiguous
		}
	}

	if (ret <= 0)
		close(fd);
	return ret;
}


static int	broadcast_msg(client *clients, int n, int sender_id,
	const char *msg, size_t len)
{
	char		prefix[19];
	ssize_t		prefix_len;
	int			ret = 0;

	if (sender_id < 0)
		prefix_len = sprintf(prefix, "server: client %d ", -sender_id);
	else
		prefix_len = sprintf(prefix, "client %d: ", sender_id);

	len += prefix_len;
	printf("prefix: %s, len: %zd, total: %zu\n", prefix, prefix_len, len);

	for (int id = 0; ret == 0 && id < n; id++)
	{
		if (clients[id].r.size != 0)
		{
			if (clients[id].w.size - clients[id].w.len < len)
				ret = buff_resize(&clients[id].w, clients[id].w.size + len + 1);
			printf("%zu:%zu\n", clients[id].w.len, clients[id].w.size);
			strcpy(clients[id].w.data + clients[id].w.len, prefix);
			strcpy(clients[id].w.data + clients[id].w.len + prefix_len, msg);
			clients[id].w.len += len;
		}
	}
	return ret;
}

static int  listener_accept(client *clients, int listen_fd, int *highest_fd)
{
	struct sockaddr_in	addr;
	socklen_t			addr_len = sizeof(addr);
	int					fd
		= accept(listen_fd, (struct sockaddr*)&addr, &addr_len);

	printf("Accepting connection...\n");

	if (fd != -1)
	{
		if (buff_resize(&clients[fd - listen_fd - 1].r, 4096) == -1)
		{
			fd = -1;
			close(fd);
		}
		else
		{
			if (fd > *highest_fd)
				*highest_fd = fd;
			broadcast_msg(clients, *highest_fd - listen_fd,
				-(fd - listen_fd - 1), "just arrived\n", 13);
		}
	}

	return fd;
}

static int	listener_new(int port)
{
	int	listen_fd = socket_new();

	if (listen_fd != -1
	&& (socket_bind(listen_fd, port) != 0 || listen(listen_fd, 0) != 0))
	{
		close(listen_fd);
		listen_fd = -1;
	}
	return listen_fd;
}

static int	listener_select(client *clients, int listen_fd, int highest_fd,
	fd_set *rfds, fd_set *wfds)
{
	int	status;

	FD_ZERO(rfds);
	FD_ZERO(wfds);

	FD_SET(listen_fd, rfds);

	for (int id = 0; id != highest_fd - listen_fd; id++)
	{
		if (clients[id].r.size != 0)
			FD_SET(listen_fd + 1 + id, rfds);
		if (clients[id].w.len != 0)
			FD_SET(listen_fd + 1 + id, wfds);
	}

	printf("Waiting for connections...\n");
	status = select(highest_fd + 1, rfds, wfds, NULL, NULL);

	printf("status: %d\n", status);
	if (status == -1 && errno == EINTR)
		status = 0;

	return status;
}

static int	listener_hfd(client *clients, int listener_fd, int highest_fd)
{
	int new_highest = -1;

	for (int id = highest_fd - listener_fd - 1; id != -1; id--)
		if (clients[id].r.size != 0)
			new_highest = id;

	return listener_fd + 1 + new_highest;
}

#include <sys/fcntl.h>

static int	listener_loop(int fd)
{
	static client		c[64];
	fd_set				rfds;
	fd_set				wfds;
	int					hfd = fd;
	int					s;

fcntl(fd, F_SETFL, O_NONBLOCK);
	while ((s = listener_select(c, fd, hfd, &rfds, &wfds)) > 0)
	{
		if (FD_ISSET(fd, &rfds))
			listener_accept(c, fd, &hfd);

		for (int id = 0; id < hfd - fd; id++)
		{
			if (c[id].r.size != 0)
			{
				const int	cfd = fd + 1 + id;

				s = cli_io(c + id, cfd, FD_ISSET(cfd, &rfds), FD_ISSET(cfd, &wfds));
				if (s == 0)
				{
					if (cfd == hfd)
						hfd = listener_hfd(c, fd, hfd - 1);

					buff_clr(&c[id].r);
					buff_clr(&c[id].w);
					s = broadcast_msg(c, hfd - fd, -id, "just left\n", 10);
				}
				else if (s > 1)
				{
					if (broadcast_msg(c, hfd - fd, id, c[id].r.data, s) == 0)
					{
						c[id].r.len -= s;
						ft_memmove(c[id].r.data, c[id].r.data + s, c[id].r.len);
						s = 1;
					}
					else
						s = -1;
				}
			}
		}		
	}
	for (int id = 0; id < hfd - fd; id++)
	{
		close(fd + 1 + id);
		buff_clr(&c[id].r);
		buff_clr(&c[id].w);
	}
	return s;
}

int main(int ac, const char **av)
{
	int	err;
	int listen_fd;

	if (ac >= 2)
	{
		listen_fd = listener_new(atoi(av[1]));
		if (listen_fd == -1)
			err = fatal_error();
		else if (listener_loop(listen_fd) == -1)
		{
			err = fatal_error();
			close(listen_fd);
		}
		else
			err = 0;
	}
	else
		err = invalid_arguments();
	return err;
}