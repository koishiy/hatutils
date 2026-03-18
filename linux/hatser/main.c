#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../api.h"

#define PORT 16834

// This file currently only works for LiveSplit one.
// It provides a web server that sends data towards the program.

struct __attribute__((packed)) hat_timer {
    u32 start_magic;
    u32 timer_state;
    f64 unpause_time;
    u32 game_timer_is_paused;
    u32 act_timer_is_paused;
    u32 act_timer_is_visible;
    u32 unpause_time_is_dirty;
    u32 just_got_time_piece;
    f64 game_time;
    f64 act_time;
    f64 real_game_time;
    f64 real_act_time;
    u32 time_piece_count;
    u32 end_magic;
};

struct hat_save_data {
    u32 yarn;
    u32 lifetime_yarn;
    u32 pons;
    u32 timepieces;
    u32 mod_timepieces;
    u32 badge_pins;
    u32 chapter;
    u32 act;
    u32 checkpoint;
};

struct vector {
    f32 x;
    f32 y;
    f32 z;
};

struct region {
    u64 start;
    u64 end;
};

s32 hat_pid;
void* timer_ptr; // the address in HatinTimeGame.exe
struct hat_timer timer;
struct hat_timer old_timer;

// updates timer_ptr, returning positive on success
u8 find_timer(s32 pid) {
    char buf[256];
    struct region region;

    snprintf(buf, sizeof(buf), "pmap -A 0x140000000,0x150000000 %d | grep 'K rw---'", pid);

    FILE* cmd = popen(buf, "r");

    u64 region_size;
    char suffix;
    fscanf(cmd, "%lX %lu%c", &region.start, &region_size, &suffix);
    pclose(cmd);

    // usually K
    if(suffix == 'K') {
        region_size *= 1024;
    }
    // i don't even know if this occurs in pmap
    else if(suffix == 'M') {
        region_size *= 1024 * 1024;
    }

    region.end = region.start + region_size;

    puts("Searching for timer in memory...");
    // dumb, simple search
    u8 found = 0;
    for(u64 ofs = region.start; ofs < region.end; ofs += 4) {
        // TIMR
        if(read_u32(pid, (void*)ofs) == 0x524D4954) {
            // just to make sure
            read_bytes(pid, (void*)ofs, sizeof(timer), &timer);

            // now we are sure
            if(timer.start_magic == 0x524D4954 && timer.end_magic == 0x20444E45) {
                timer_ptr = (void*)ofs;
                printf("Found timer at %p\n", timer_ptr);
                printf("Autosplitter attached.");

                found = 1;
                break;
            }
        }
    }

    return found;
}

u32 ip;
u16 port;
s32 socket_fd;
s32 sockets_fd;

int server_fd;
int client_fd;

// web socket protocol bs
void get_websocket_accept(char *key, char *output)
{
    char buffer[256];

    sprintf(buffer,"%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11",key);

    unsigned char hash[SHA_DIGEST_LENGTH];

    SHA1((unsigned char*)buffer,strlen(buffer),hash);

    EVP_EncodeBlock((unsigned char*)output,hash,SHA_DIGEST_LENGTH);
}

u8 create_webserver() {
    struct sockaddr_in addr;
    char buffer[4096];

    server_fd = socket(AF_INET,SOCK_STREAM,0);

    int opt=1;
    setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(PORT);

    bind(server_fd,(struct sockaddr*)&addr,sizeof(addr));
    listen(server_fd,5);

    puts("Created web server. Waiting for LiveSplit One...");

    return 1;
}

u8 wait_for_livesplit() {
    char buffer[4096];
    client_fd=accept(server_fd,NULL,NULL);

    int n=read(client_fd,buffer,sizeof(buffer)-1);
    buffer[n]=0;

    char *key_start=strstr(buffer,"Sec-WebSocket-Key:");

    if(!key_start){
        close(client_fd);
        return 0;
    }

    key_start+=19;

    char key[128];

    sscanf(key_start,"%s",key);

    char accept[128];

    get_websocket_accept(key,accept);

    char response[512];

    sprintf(response,
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "\r\n",accept);

    write(client_fd,response,strlen(response));

    printf("LiveSplit connected!\n");

    return 1;
}


// send data
void ws_send(char *msg){
    if(client_fd <= 0)
        return;

    int len = strlen(msg);

    if(len > 1000){
        puts("Message too big");
        return;
    }

    unsigned char frame[1024];

    frame[0] = 0x81;
    frame[1] = len;

    memcpy(frame+2,msg,len);

    send(client_fd,frame,len+2,0);
}

// helper function to test what the FUG livesplit was sending back
/*void ws_recv()
{
    unsigned char buf[512];

    int n = recv(client_fd,buf,sizeof(buf),MSG_DONTWAIT);

    if(n <= 0)
        return;

    int opcode = buf[0] & 0x0F;

    int masked = buf[1] & 0x80;
    int len = buf[1] & 0x7F;

    if(opcode == 1){ // text frame

        int offset = 2;

        unsigned char mask[4];

        if(masked){

            memcpy(mask,buf+offset,4);
            offset += 4;

            char msg[512];

            for(int i=0;i<len;i++){
                msg[i] = buf[offset+i] ^ mask[i%4];
            }

            msg[len]=0;

            printf("LiveSplit says: %s\n",msg);
        }
    }

    // ping
    if(opcode == 9){

        unsigned char pong[2];

        pong[0]=0x8A;
        pong[1]=0;

        send(client_fd,pong,2,0);
    }
}*/

u8 should_start() {
    return timer.timer_state == 1 && old_timer.timer_state == 0;
}

u8 should_reset() {
    return timer.timer_state == 0 && old_timer.timer_state == 1;
}

u8 should_split() {
    return (timer.time_piece_count == old_timer.time_piece_count + 1 && timer.act_timer_is_visible == 1) || timer.timer_state == 2;
}

void send_game_time(){
    if(timer.real_game_time != old_timer.real_game_time) {
        return;
    }

    double t = timer.real_game_time;

    int hours = (int)(t / 3600);
    int minutes = ((int)t % 3600) / 60;
    int seconds = (int)t % 60;

    int millis = (int)((t - (int)t) * 1000.0);

    char timestr[64];

    snprintf(
        timestr,
        sizeof(timestr),
             "%02d:%02d:%02d.%03d",
             hours,
             minutes,
             seconds,
             millis
    );

    char gtbuf[128];

    snprintf(
        gtbuf,
        sizeof(gtbuf),
             "{\"command\":\"setGameTime\",\"time\":\"%s\"}",
             timestr
    );

    ws_send(gtbuf);
}

int main(int argc, char** argv) {
    while(1) {
        create_webserver();

        wait_for_livesplit();

        hat_pid = pid_from_name("HatinTimeGame.exe");
        while(hat_pid == -1) {
            puts("Unable to find HatinTimeGame.exe process, retrying in a few seconds...");
            sleep(5);
            hat_pid = pid_from_name("HatinTimeGame.exe");
        }

        if(find_timer(hat_pid)) {
            // main autosplitter loop
            while(1) {
                //ws_recv();

                old_timer = timer;
                read_bytes(hat_pid, timer_ptr, sizeof(timer), &timer);

                // failure to read process memory
                if(timer.start_magic == 0) {
                    break;
                }

                send_game_time();

                if(should_start()) {
                    ws_send("{\"command\":\"start\"}");
                }

                if(should_split()) {
                    ws_send("{\"command\":\"split\"}");
                }

                if(should_reset()) {
                    ws_send("{\"command\":\"reset\",\"saveAttempt\":true}");
                }

                // TODO: make configurable
                usleep(5000);
            }
        }
        else {
            puts("Found HatinTimeGame.exe, but could not find timer, retrying in a few seconds...");
            sleep(5);
            continue;
        }
    }

    return 0;
}
