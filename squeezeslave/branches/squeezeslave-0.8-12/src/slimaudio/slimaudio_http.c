/*
 *   SlimProtoLib Copyright (c) 2004,2006 Richard Titmuss
 *
 *   This file is part of SlimProtoLib.
 *
 *   SlimProtoLib is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   SlimProtoLib is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with SlimProtoLib; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#include <stdio.h>
#include <string.h>
#include <pthread.h>


#ifdef __WIN32__
  #include <winsock.h>
  #define CLOSESOCKET(s) closesocket(s)
#else
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #define CLOSESOCKET(s) close(s)
#endif


#include "slimaudio/slimaudio.h"

#define HTTP_HEADER_LENGTH 1024


#ifdef SLIMPROTO_DEBUG
  #define DEBUGF(...) if (slimaudio_http_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_http_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif


bool slimaudio_http_debug;
bool slimaudio_http_debug_v;


static void *http_thread(void *ptr);
static void http_recv(slimaudio_t *a);
static void http_close(slimaudio_t *a);


int slimaudio_http_open(slimaudio_t *audio) {
	pthread_mutex_init(&(audio->http_mutex), NULL);
	pthread_cond_init(&(audio->http_cond), NULL);
	/* 
	 * We lock the mutex right here, knowing that http_thread will
	 * release it once it enters pthread_cond_wait inside its STOPPED
	 * state.
	 */
	pthread_mutex_lock(&audio->http_mutex);

	if (pthread_create( &audio->http_thread, NULL, http_thread, (void*) audio) != 0) {
		fprintf(stderr, "Error creating http thread\n");
		pthread_mutex_unlock(&audio->http_mutex);
		return -1;
	}

	return 0;
}


int slimaudio_http_close(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->http_mutex);

	audio->http_state = STREAM_QUIT;	
	pthread_cond_broadcast(&audio->http_cond);
	
	pthread_mutex_unlock(&audio->http_mutex);
	
	pthread_join(audio->http_thread, NULL);	

	pthread_mutex_destroy(&(audio->http_mutex));
	pthread_cond_destroy(&(audio->http_cond));
	return 0;
}


static void *http_thread(void *ptr) {
	slimaudio_t *audio = (slimaudio_t *) ptr;
#ifdef SLIMPROTO_DEBUG				
	int last_state = 0;
#endif

	audio->http_state = STREAM_STOPPED;
	
	while (true) {
				
#ifdef SLIMPROTO_DEBUG				
		if (last_state == audio->http_state) {
			VDEBUGF("http_thread state %i\n", audio->http_state);
		}
		else {
			DEBUGF("http_thread state %i\n", audio->http_state);
		}
			
		last_state = audio->http_state;
#endif

		switch (audio->http_state) {
			case STREAM_STOP:
				CLOSESOCKET(audio->streamfd);
				slimproto_dsco(audio->proto, DSCO_CLOSED);

				slimaudio_buffer_close(audio->decoder_buffer);
				
				audio->http_state = STREAM_STOPPED;
				pthread_cond_broadcast(&audio->http_cond);
				break;
				
			case STREAM_STOPPED:
				pthread_cond_wait(&audio->http_cond, &audio->http_mutex);
				break;
			
			case STREAM_PLAYING:
				pthread_mutex_unlock(&audio->http_mutex);
				
				http_recv(audio);
				
				pthread_mutex_lock(&audio->http_mutex);
				break;
				
			case STREAM_QUIT:
				return 0;
		}
	}
		
	pthread_mutex_unlock(&audio->http_mutex);
}


void slimaudio_http_connect(slimaudio_t *audio, slimproto_msg_t *msg) {
	slimaudio_http_disconnect(audio);
	
	struct sockaddr_in serv_addr = audio->proto->serv_addr;
	if (msg->strm.server_ip != 0) {
		serv_addr.sin_addr.s_addr = htonl(msg->strm.server_ip);
	}
	if (msg->strm.server_port != 0) {
		serv_addr.sin_port = htons(msg->strm.server_port);
	}
	
	DEBUGF("slimaudio_http_connect: http connect %s:%i\n", 
	       inet_ntoa(serv_addr.sin_addr), msg->strm.server_port);
	
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("Error opening socket");
		return;
	}

	if (connect(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) != 0) {
		fprintf(stderr, "slimaudio_http_connect: "
			"error connecting to server\n");
		CLOSESOCKET(fd);
		return;
	}

	if (slimproto_configure_socket(fd) != 0) {
		fprintf(stderr, "slimaudio_http_connect: "
			"error configuring socket.\n");
		CLOSESOCKET(fd);
		return;
	}

	/* send http request to server */
	DEBUGF("slimaudio_http_connect: http request %s\n", msg->strm.http_hdr);
	int n = send(fd, msg->strm.http_hdr, strlen(msg->strm.http_hdr), 
		     slimproto_get_socketsendflags());
	if (n < 0) {
		fprintf(stderr, "slimaudio_http_connect: "
			"error reading from socket\n");
		CLOSESOCKET(fd);	
	}

	/* read http header */
	char http_hdr[HTTP_HEADER_LENGTH];
	int pos = 0;
	int crlf = 0;
	
	do {
		n = recv(fd, http_hdr+pos, 1, 0);
		if (n < 0) {
			fprintf(stderr, "slimaudio_http_connect: "
				"error reading from socket\n");
			CLOSESOCKET(fd);	
		}
		
		switch (crlf) {
			case 0:	
			case 2:	
				if (http_hdr[pos] == 13) {
					crlf++;
				}
				else {
					crlf = 0;
				}
				break;
			case 1:
			case 3:
				if (http_hdr[pos] == 10) {
					crlf++;
				}
				else {
					crlf = 0;
				}
				break;
			default:
				crlf = 0;
				break;			
		}
		
		pos++;
	} while (crlf < 4 && pos < HTTP_HEADER_LENGTH -1);
	http_hdr[pos+1] = '\0';
		
	DEBUGF("slimaudio_http_connect: http connected hdr %s\n", http_hdr);
	
	pthread_mutex_lock(&audio->http_mutex);

	slimaudio_buffer_open(audio->decoder_buffer, NULL);	
	
	audio->streamfd = fd;
	audio->http_stream_bytes = 0;
	audio->autostart = 
		msg->strm.autostart == '1' || msg->strm.autostart == '3';
	audio->autostart_threshold = (msg->strm.threshold & 0xFF) * 1024;
	
	DEBUGF("slimaudio_http_connect: autostart=%i autostart_threshold=%i\n",
	       audio->autostart, audio->autostart_threshold);
	
	audio->http_state = STREAM_PLAYING;
	pthread_cond_broadcast(&audio->http_cond);

	pthread_mutex_unlock(&audio->http_mutex);	
}

void slimaudio_http_disconnect(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->http_mutex);

	if (audio->http_state == STREAM_PLAYING) {
		DEBUGF("slimaudio_http_disconnect: state=%i\n", audio->http_state);

		audio->http_state = STREAM_STOP;
		pthread_cond_broadcast(&audio->http_cond);
		
		/* closing socket and buffer will wake the http thread */
		CLOSESOCKET(audio->streamfd);
		slimaudio_buffer_close(audio->decoder_buffer);		
		
		while (audio->http_state == STREAM_STOP) {
			pthread_cond_wait(&audio->http_cond, &audio->http_mutex);
		}
	}
	pthread_mutex_unlock(&audio->http_mutex);	
}

static void http_recv(slimaudio_t *audio) {
	char buf[AUDIO_CHUNK_SIZE];

	int n = recv(audio->streamfd, buf, AUDIO_CHUNK_SIZE, 0);

	if (n == 0) {
		DEBUGF("http_recv: http eof\n");
		http_close(audio);
		return;	
	}
	
	if (n < 0) {
		fprintf(stderr, "http_recv: error reading from socket\n");
		http_close(audio);
		return;
	}

	VDEBUGF("http_recv: audio n=%i\n", n);
	slimaudio_buffer_write(audio->decoder_buffer, buf, n);
	
	pthread_mutex_lock(&audio->http_mutex);
	
	audio->http_total_bytes += n;
	audio->http_stream_bytes += n;
	
	if (audio->autostart && audio->http_stream_bytes > audio->autostart_threshold) {
		DEBUGF("http_recv: AUTOSTART at %i\n", audio->http_stream_bytes);
		audio->autostart = false;
		pthread_mutex_unlock(&audio->http_mutex);
		
		slimaudio_output_unpause(audio);
	}
	else {
		pthread_mutex_unlock(&audio->http_mutex);
	}
}


static void http_close(slimaudio_t *audio) {
	pthread_mutex_lock(&audio->http_mutex);
	
	audio->http_state = STREAM_STOP;		
	pthread_cond_broadcast(&audio->http_cond);						
	
	pthread_mutex_unlock(&audio->http_mutex);	
}
