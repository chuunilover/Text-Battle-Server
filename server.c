/*
 * socket demonstrations:
 * This is the server side of an "internet domain" socket connection, for
 * communicating over the network.
 *
 * In this case we are willing to wait either for chatter from the client
 * _or_ for a new connection.
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#ifndef PORT
#define PORT 30100
#endif

struct game* games;
// modified this to support games; noncanonical mode message typing
struct client {
    int fd;
    // player name
    char* name;
    // buffer for storing current message (for noncanonical mode players)
    char curmessage[256];
    struct in_addr ipaddr;
    // what game this player is in (NULL for none)
    struct game* curgame;
    struct client* next;
    // who this guy last played against; NULL if match not yet played or last played against player who left
    struct client* lastplayed;
};

// linked list struct for managing games
struct game {
    struct client* players[2];
    int turn; // 0 or 1
    int powermoves[2];
    int hp[2];
    struct game* next;
    // modes: 0: waiting for attack, 1: attacking, 2: waiting for chat; 3 for powermove
    char mode;
};

static struct client* addclient(struct client* top, int fd, struct in_addr addr);
static struct client* removeclient(struct client* top, int fd);
//static void broadcast(struct client* top, char* s, int size);
int handleclient(struct client* p, struct client* top);
static struct game* matchmake(struct client* top, struct game* games);
static void broadcast_most(struct client* p, char* s, int size, struct client* exclude);
struct game * handle_games(struct game* top);
struct game * removegame(struct game *top, struct game *rem);
struct client *pushtoback(struct client *top, struct client *topush);

int bindandlisten(void);

int main(void)
{
    int clientfd, maxfd, nready;
    struct client* p;
    struct client* head = NULL;
    socklen_t len;
    struct sockaddr_in q;
    struct timeval tv;
    fd_set allset;
    fd_set rset;

    games = NULL;

    int i;

    int listenfd = bindandlisten();
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while(1) {
	// make a copy of the set before we pass it into select
	rset = allset;
	/* timeout in seconds (You may not need to use a timeout for
	* your assignment)*/
	tv.tv_sec = 10;
	tv.tv_usec = 0; /* and microseconds */

	nready = select(maxfd + 1, &rset, NULL, NULL, &tv);
	if(nready == 0) {
	    printf("No response from clients in %ld seconds\n", tv.tv_sec);
	    continue;
	}

	if(nready == -1) {
	    perror("select");
	    continue;
	}

	if(FD_ISSET(listenfd, &rset)) {
	    printf("a new client is connecting\n");
	    len = sizeof(q);
	    if((clientfd = accept(listenfd, (struct sockaddr*)&q, &len)) < 0) {
		perror("accept");
		exit(1);
	    }
	    FD_SET(clientfd, &allset);
	    if(clientfd > maxfd) {
		maxfd = clientfd;
	    }
	    printf("connection from %s\n", inet_ntoa(q.sin_addr));
	    head = addclient(head, clientfd, q.sin_addr);
	    if (write(head->fd, "What is your name?", sizeof("What is your name?")) == -1){
			perror("write");
			exit(1);
		}
	}

	for(i = 0; i <= maxfd; i++) {
	    if(FD_ISSET(i, &rset)) {
		for(p = head; p != NULL; p = p->next) {
		    if(p->fd == i) {
			int result = handleclient(p, head);
			if(result == -1) {
			    int tmp_fd = p->fd;
			    head = removeclient(head, p->fd);
			    FD_CLR(tmp_fd, &allset);
			    close(tmp_fd);
			}
			break;
		    }
		}
	    }
	}
	games = matchmake(head, games);
	games = handle_games(games);
	head = pushtoback(head, head);
    }
    return 0;
}

/* set p -> name to name */
int setname(struct client* p, char* name)
{
	//up to 40 bits for name
    if (!(p->name = malloc(40))){
		perror("malloc");
		exit(1);
	}
	p -> name[39] = '\0'; //set last char to null char to indicate end of string
    strncpy(p->name, name, strlen(name));
    int i = 0;
	/* sets all newline and network newline chars to null terminator*/
    while(p->name[i] != '\0') {
		if(p->name[i] == '\r' || p->name[i] == '\n') {
			p->name[i] = '\0';
		}
		i++;
    }
    return 0;
}

int handleclient(struct client* p, struct client* top)
{
    char buf[256];
    char outbuf[512];
    int len = read(p->fd, buf, sizeof(buf) - 1);
    if(len > 0) {
		buf[len] = '\0';
		if(len == 1 && buf[0] != '\n' && buf[0] != '\r') {
			int i = strlen(p->curmessage);
			if(i + len < 255) {
				//add to message buffer
				p->curmessage[i] = buf[0];
				p->curmessage[i + 1] = '\0';
			} else {
				//set last few chars to line termination and string termination
				p->curmessage[253] = '\r';
				p->curmessage[254] = '\n';
				p->curmessage[255] = '\0';
			}
			if(p->name) {
				/* If not in game, or in game and not your turn, delete chars typed from message buffer */
				if(!p->curgame || (p->curgame && p->curgame->players[p->curgame->turn]->fd != p->fd)) {
					printf("command rejected from %s!\n", p->name);
					p -> curmessage[0] = '\0';

				}
				/* If in game, is your turn and game waiting for command, process and accept commands, reset message buffer */
				else if(p->curgame && p->curgame->players[p->curgame->turn]->fd == p->fd && p->curgame->mode == 4) {
					if(p->curmessage[0] == 'a') {
						p->curgame->mode = 1;
						p->curmessage[0] = '\0';
					} else if(p->curmessage[0] == 's') {
						p->curgame->mode = 2;
						if (write(p -> fd, "\r\nSay something...\r\n",20) == -1){
							perror("write");
							exit(1);
						}
						p->curmessage[0] = '\0';
						printf("%s\n", p -> curmessage);
					} else if(p->curmessage[0] == 'p' && p->curgame->powermoves[p->curgame->turn]) {
						p->curgame->mode = 3;
						p->curmessage[0] = '\0';
					} else {
						p->curmessage[0] = '\0';
					}
				}
			}
		} else {
			/* copy current message into message buffer */
			strncpy(p->curmessage + strlen(p->curmessage), buf, len);
			if(p->name) {
				/* reject command; same conditions as above */
				if(!p->curgame || (p->curgame && p->curgame->players[p->curgame->turn]->fd != p->fd)) {
					printf("command rejected from %s!\n", p->name);
					p->curmessage[0] = '\0';
				}
				/* accept commands, process (or not) based on first char in mesage buffer; reset message buffer */
				else if(p->curgame && p->curgame->players[p->curgame->turn]->fd == p->fd && p->curgame->mode == 4) {
					if(p->curmessage[0] == 'a') {
							p->curgame->mode = 1;
							p->curmessage[0] = '\0';
					} else if(p->curmessage[0] == 's') {
						p->curgame->mode = 2;
						if(write(p -> fd, "\r\nSay something...\r\n",20) == -1){
							perror("write");
							exit(1);
						}
						p->curmessage[0] = '\0';
						printf("%s\n", p -> curmessage);
					} else if(p->curmessage[0] == 'p' && p->curgame->powermoves[p->curgame->turn]) {
						p->curgame->mode = 3;
						p->curmessage[0] = '\0';
					} else {
						p->curmessage[0] = '\0';
					}
				}
				/* if p is in game and in chat mode, send message from message buffer for both players in game to see */
				else if(p -> curgame && p->curgame->players[p->curgame->turn]->fd == p->fd && p->curgame->mode == 2) {
					char msg[330];
					sprintf(msg, "\r\n%s takes a break to tell you: %s\r\n", p->curgame->players[p->curgame->turn] -> name, p -> curmessage);
					if (write(p -> curgame -> players[(p -> curgame -> turn + 1) % 2] -> fd, msg, strlen(msg)) == -1){
						perror("write");
						exit(1);
					}
					sprintf(msg, "\r\nYou say: %s", p -> curmessage);

					p -> curgame -> mode = 0;
					p -> curmessage[0] = '\0';
				}
			} 
		/* name not yet set; not "in arena" */
		else {
			/* set up name, read of message buffer; limit name size to 40 chars */
			p->curmessage[40] = '\0';
			setname(p, p->curmessage);
			/* broadcast and send to new client appropriate messages */
			char welcome_msg[160];
			sprintf(welcome_msg, "\n**%s enters the arena...**\r\n", p->name);
			broadcast_most(top, welcome_msg, strlen(welcome_msg), p);
			sprintf(welcome_msg, "Welcome, %s! Awaiting opponent...\r\n", p->name);
			if (write(p->fd, welcome_msg, strlen(welcome_msg)) == -1){
				perror("write");
				exit(1);
			}
	    }
	    p->curmessage[0] = '\0';
	    return 0;
		}
    } else if(len == 0) {
	// socket is closed
	printf("Disconnect from %s\n", inet_ntoa(p->ipaddr));
	sprintf(outbuf, "Goodbye %s\r\n", inet_ntoa(p->ipaddr));
	//broadcast(top, outbuf, strlen(outbuf));
	return -1;
    } else { // shouldn't happen
	perror("read");
	return -1;
    }
    return 0;
}

/* bind and listen, abort on error
 * returns FD of listening socket
 */
int bindandlisten(void)
{
    struct sockaddr_in r;
    int listenfd;

    if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	perror("socket");
	exit(1);
    }
    int yes = 1;
    if((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
	perror("setsockopt");
    }
    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(PORT);

    if(bind(listenfd, (struct sockaddr*)&r, sizeof r)) {
	perror("bind");
	exit(1);
    }

    if(listen(listenfd, 5)) {
	perror("listen");
	exit(1);
    }
    return listenfd;
}

static struct client* addclient(struct client* top, int fd, struct in_addr addr)
{
    struct client* p = malloc(sizeof(struct client));
    if(!p) {
	perror("malloc");
	exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
	//SETTING EVERYTHING NULL
    p->name = NULL;
    p->next = top;
    p->lastplayed = NULL;
    p->curgame = NULL;
    int i;
    for(i = 0; i < 256; i++) {
	p->curmessage[i] = '\0';
    }
	//ADDED CODE ENDS HERE
    top = p;
    return top;
}

static struct client* removeclient(struct client* top, int fd)
{
    struct client** p;

    for(p = &top; *p && (*p)->fd != fd; p = &(*p)->next)
	;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if(*p) {
		struct client* t = (*p)->next;
		printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
		// free p's name if p had one; name was malloc'd
		if ((*p) -> name){
			free((*p) -> name);
			(*p) -> name = NULL;
		}
		// end p's game, give victory message to p's opponent, delete game, set p's opponent to be recognized by matchmake function
		if ((*p) -> curgame){
			struct game * tofree = (*p) -> curgame;
			(*p) -> curgame -> players[0] -> lastplayed = NULL;
			(*p) -> curgame -> players[1] -> lastplayed = NULL;
			if((*p) -> curgame -> players[0] == (*p)) {
				if (write((*p) -> curgame -> players[1] -> fd, "Your opponent is a coward and left the game. You win!\r\nfinding a new opponent...\r\n",83) == -1){
					perror("write");
					exit(1);
				}
				(*p) -> curgame -> players[1] -> curgame = NULL;
			}
			else{
				if (write((*p) -> curgame -> players[0] -> fd, "Your opponent is a coward and left the game. You win!\r\nfinding a new opponent...\r\n",83) == -1){
					perror("write");
					exit(1);
				}
				(*p) -> curgame -> players[0] -> curgame = NULL;
			}
			games = removegame(games, tofree);
		}
		free(*p);
		*p = t;
    } else {
		fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n", fd);
    }
    return top;
}

/* not used in assignment, but included in sample server
static void broadcast(struct client* top, char* s, int size)
{
    struct client* p;
    for(p = top; p; p = p->next) {
		if (write(p->fd, s, size) == -1){
			perror("write");
			exit(1);
		}
    }
    should probably check write() return value and perhaps remove client
}
*/

/* broadcast function from the starter code, except sending messages to everybody but client exclude */
static void broadcast_most(struct client* top, char* s, int size, struct client* exclude)
{
    struct client* p;
    for(p = top; p; p = p->next) {
		if(p != exclude) {
			if (write(p->fd, s, size) == -1){
				perror("write");
				exit(1);
			}
		}
    }
    /* should probably check write() return value and perhaps remove client */
}

/* searches through all clients to see if they've joined a game or not, and joins a pair of waiting clients together
 returns new head of games */
static struct game* matchmake(struct client* top, struct game* games)
{
    struct client* iteration = top;
    struct client* newplayers[2];
	//making sure both pointers are null in case garbage values are in memory
    newplayers[0] = NULL;
    newplayers[1] = NULL;
    while(iteration) {
		//if name set and not in game
		if(!iteration->curgame && iteration->name) {
			//if player 1 not yet found, keep track of iteration as player 1
			if(!newplayers[0]) {
				newplayers[0] = iteration;
			}
			// if player1 has been found, this will be player 2
			// create new game, set all variables of new game, send start game messages
			else if(!newplayers[1] && iteration->lastplayed != newplayers[0]) {
				newplayers[1] = iteration;
				struct game* newgame = malloc(sizeof(struct game));
				if (!newgame){
					perror("malloc");
					exit(1);
				}
				newgame->players[0] = newplayers[0];
				newgame->players[1] = newplayers[1];
				newgame->players[0]->lastplayed = newplayers[1];
				newgame->players[1]->lastplayed = newplayers[0];
				newgame->turn = rand() % 2;
				newgame->next = games;
				newgame->mode = 0;
				newgame->hp[0] = rand() % 11 + 20;
				newgame->hp[1] = rand() % 11 + 20;
				newgame->powermoves[0] = rand() % 2 + 1;
				newgame->powermoves[1] = rand() % 2 + 1;
				newplayers[0]->curgame = newgame;
				newplayers[1]->curgame = newgame;
				char msg[256];
				sprintf(msg, "You engage %s!\r\n", newplayers[0]->name);
				if (write(newplayers[1]->fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				sprintf(msg, "You engage %s!\r\nY", newplayers[1]->name);
				if (write(newplayers[0]->fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				games = newgame;
				return games;
			}
		}
		iteration = iteration->next;
    }
	//if no new game made, return old game head pointer
    return games;
}

// Handles ALL the games, from top to the end of the linked list
// modes: 0: display move msgs, 1: attacking, 2: waiting for chat input; 3 for powermove; 4 waiting for attack
struct game *handle_games(struct game* top)
{
	/*iterate through all games*/
    struct game* cur = top;
    char msg[256];
    for(cur = top; cur != NULL; cur = cur->next) {
		/* Deal damage, send appropriate messages
		(turn + 1) % 2 is index of non-moving player*/
		if(cur -> mode == 1) {
			int i = rand() % 5 + 2;
			cur -> hp[(cur->turn + 1) % 2] -= i;
			sprintf(msg, "You hit %s for %d damage!\r\n", cur -> players[(cur->turn + 1) % 2]->name, i);
			if (write(cur->players[cur->turn]->fd, msg, strlen(msg)) == -1){
				perror("write");
				exit(1);
			}
			sprintf(msg, "You got hit by %s for %d damage!\r\n", cur -> players[cur -> turn] -> name, i);
			if (write(cur->players[(cur -> turn + 1) % 2] -> fd, msg, strlen(msg)) == -1){
				perror("write");
				exit(1);
			}
			cur -> turn = (cur->turn + 1) % 2;
			cur -> mode = 0;
		}
		/*Powermove. Similar code as above, but hit/miss must also be calculated.*/
		if(cur -> mode == 3) {
			/*Missed! Simply print messages and switch turns and mode back to wait messaging.*/
			if(rand()% 2 == 0){
				if (write(cur -> players[cur -> turn] -> fd, "\r\nYou missed!\r\n", 15) == -1){
					perror("write");
					exit(1);
				}
				sprintf(msg, "You evaded %s!", cur -> players[cur -> turn] -> name);
				if (write(cur -> players[(cur -> turn + 1) % 2] -> fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				cur -> powermoves[cur -> turn] --;
				cur->turn = (cur->turn + 1) % 2;
				cur -> mode = 0;
			}
			/* Hit! Same as above, except calculating and subtracting damage too. */
			else{
				int i = rand() % 5 + 2;
				cur->hp[(cur->turn + 1) % 2] -= i * 3;
				sprintf(msg, "You hit %s for %d damage!\r\n", cur->players[(cur->turn + 1) % 2]->name, i * 3);
				if (write(cur->players[cur->turn]->fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				cur -> powermoves[cur -> turn] --;
				cur->turn = (cur->turn + 1) % 2;
				cur->mode = 0;
			}
		}
		/* Wait messages */
		if(cur->mode == 0) {
			/* If someone died (hp < 0), send appropriate messages, deallocate game memory,
			indicate that both players are no longer in a game */
			if(cur -> hp[0] <= 0 || cur -> hp[1] <= 0){
				sprintf(msg, "\r\nYou are no match for %s. You got pwnt...\r\nFinding a new opponent...\r\n", cur -> players[(cur -> turn + 1) % 2] -> name);
				if (write(cur -> players[cur -> turn] -> fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				sprintf(msg, "\r\n%s has surrendered. You win!\r\nFinding a new opponent...\r\n", cur -> players[cur -> turn] -> name);
				if (write(cur -> players[(cur -> turn + 1) % 2] -> fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				cur -> players[0] -> curgame = NULL;
				cur -> players[1] -> curgame = NULL;
				top = removegame(top, cur);
			}
			/* Write and send player info (hp, powermoves left, etc) to client players*/
			else{
				sprintf(msg,
					"Your hitpoints: %d\r\nYour powermoves: %d \r\n\r\n%s's hitpoints: %d \r\n",
					cur->hp[cur->turn],
					cur->powermoves[cur->turn],
					cur->players[(cur->turn + 1) % 2]->name,
					cur->hp[(cur->turn + 1) % 2]);
				if (write(cur->players[cur->turn]->fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				sprintf(msg,
						"Your hitpoints: %d\r\nYour powermoves: %d \r\n\r\n%s's hitpoints: %d \r\n",
						cur->hp[(cur->turn + 1) % 2],
						cur->powermoves[(cur->turn + 1) % 2],
						cur->players[cur->turn]->name,
						cur->hp[cur->turn]);
				if (write(cur->players[(cur->turn + 1) % 2]->fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
				if(cur->powermoves[cur->turn]) {
					if (write(cur->players[cur->turn]->fd, "\r\n(a)ttack\r\n(p)owermove \r\n(s)peak something \r\n", 47) == -1){
						perror("write");
						exit(-1);
					}
				} else {
					if (write(cur->players[cur->turn]->fd, "\r\n(a)ttack\r\n(s)peak something \r\n", 33) == -1){
						perror("write");
						exit(-1);
					}
				}
				sprintf(msg, "Waiting for %s to strike...\r\n", cur->players[(cur->turn) % 2]->name);
				if (write(cur->players[(cur->turn + 1) % 2]->fd, msg, strlen(msg)) == -1){
					perror("write");
					exit(1);
				}
			}
			cur->mode = 4;
			/* waiting for command mode */
		}
	}
    return top;
}

/* remove game *rem from linked list of struct games if *rem is in linked list with head top
   return new head of list*/
struct game * removegame(struct game *top, struct game *rem){
	struct game *cur = top;
	/* if we want to remove head, set head to head.next, free head, return pointer to head.next */
	if (top == rem) {
		cur = top -> next;
		free(top);
		printf("game removed\n");
		return cur;
	}
	/* don't want to remove head */
	else{
		/* iterate through all games, remove rem by linking node before rem to node after rem and freeing rem*/
		struct game *prev = NULL;
		for(cur = top; cur; cur = cur -> next){
			if(cur == rem){ 
				prev -> next = cur -> next;
				free(cur);
				printf("game removed\n");
				return top;
			}
			prev=cur;
		}
	}
	return top;
}
 
 /*pushes *topush to back of linked list with head *top ; return new head */
struct client *pushtoback(struct client *top, struct client *topush){
	struct client *curr;
	/* return null head if head is null */
	if (!top){
		return top;
	}
	/* if head pushed back, head.next is new head */
	if(topush == top){
		top = top -> next;
	}
	/* unless the head has no next */
	if (!top){
		return topush;
	}
	for(curr = top; curr && curr -> next; curr = curr -> next){} 
	//make curr point to last element in list
	curr -> next = topush;
	topush -> next = NULL;
	/* set topush as old last link's next, set last link to null 
	 * covers case where topush is last element in list */
	return top;
}