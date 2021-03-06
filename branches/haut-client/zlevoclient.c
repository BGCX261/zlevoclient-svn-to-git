/*
 * =====================================================================================
 *
 *       Filename:  zlevoclient.c
 *
 *    Description:  main source file for ZlevoClient
 *
 *        Version:  0.1
 *        Created:  05/24/2009 05:38:56 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  PT<pentie@gmail.com>
 *        Company:  http://apt-blog.co.cc
 *
 * =====================================================================================
 */

#include <pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/ethernet.h>
#ifndef __linux
//------bsd/apple mac
    #include <net/if_var.h>
    #include <net/if_dl.h>
    #include <net/if_types.h>
#endif

#include <getopt.h>
#include <iconv.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include "md5.h"

#ifndef __linux
    int bsd_get_mac(const char ifname[], uint8_t eth_addr[]);
#endif

/* ZlevoClient Version */
#define LENOVO_VER "1.1_haut"

/* default snap length (maximum bytes per packet to capture) */
#define SNAP_LEN 1518

/* ethernet headers are always exactly 14 bytes [1] */
#define SIZE_ETHERNET 14

#define LOCKFILE "/var/run/zlevoclient.pid"

#define KEEP_ALIVE_TIME 60

#define LOCKMODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

/* Ethernet header */
struct sniff_ethernet {
    u_char  ether_dhost[ETHER_ADDR_LEN];    /* destination host address */
    u_char  ether_shost[ETHER_ADDR_LEN];    /* source host address */
    u_short ether_type;                     /* IP? ARP? RARP? etc */
};

struct sniff_eap_header {
    u_char eapol_v;
    u_char eapol_t;
    u_short eapol_length;
    u_char eap_t;
    u_char eap_id;
    u_short eap_length;
    u_char eap_op;
    u_char eap_v_length;
    u_char eap_info_tailer[40];
};

enum EAPType {
    EAPOL_START,
    EAPOL_LOGOFF,
    EAP_REQUEST_IDENTITY,
    EAP_RESPONSE_IDENTITY,
    EAP_REQUEST_IDENTITY_KEEP_ALIVE,
    EAP_RESPONSE_IDENTITY_KEEP_ALIVE,
    EAP_REQUETS_MD5_CHALLENGE,
    EAP_RESPONSE_MD5_CHALLENGE,
    EAP_REQUEST_HTTP_DIGEST1,/**Add By An For Haut [Tavakoli] **/
    EAP_RESPONSE_HTTP_DIGEST1,/**Add By An For Haut [Tavakoli] **/
    EAP_REQUEST_HTTP_DIGEST2,/**Add By An For Haut [Tavakoli] **/
    EAP_RESPONSE_HTTP_DIGEST2,/**Add By An For Haut [Tavakoli] **/
    EAP_SUCCESS,
    EAP_FAILURE,
    ERROR
};

enum STATE {
   READY,
   STARTED,
   ID_AUTHED,
   ONLINE
};

void    send_eap_packet(enum EAPType send_type);
void    show_usage();
char*   get_md5_digest(const char* str, size_t len);
void    action_by_eap_type(enum EAPType pType, 
                        const struct sniff_eap_header *header);
void    init_frames();
void    init_info();
void    init_device();
void    init_arguments(int argc, char **argv);
int     set_device_new_ip();
void    fill_password_md5(u_char *attach_key, u_int id);
int     program_running_check();
void   keep_alive();
int     code_convert(char *from_charset, char *to_charset,
             char *inbuf, size_t inlen, char *outbuf, size_t outlen);
void    print_server_info (const u_char *str);
void    daemon_init(void);

static void signal_interrupted (int signo);
static void get_packet(u_char *args, const struct pcap_pkthdr *header, 
                        const u_char *packet);

u_char talier_eapol_start[] = {0x00, 0x00, 0x2f, 0xfc, 0x03, 0x00};//the last is 0x01 ,not 0x00;but 0x00 is ok#######################
u_char talier_eap_md5_resp[] = {0x00, 0x00, 0x2f, 0xfc, 0x00, 0x03, 0x01, 0x01, 0x00};
/**************一下是eap_http_digest的tailer数据内容,待验证准确性******************/
u_char tailer_eap_http_resp1[]={0x01,0x00,0x07,0x00,0x72,0x00,0x00,0x01,0x37,
0x00,0x01,0x00,0x6a,0x00,0x02,0x00,0x04,0x00,0x01,0x37,0x00,0x00,0x07,0x00,0x3f,
0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x03,0x00,0x05,0x00,0x00,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x02,0x00,0x04,0x00,0x2f,0xfc,0x00,0x01,0x91,0x00,0x13,0x00,0x00,0x00,0x05,0x00,
0x00,0x00,0x01,0x00,0x00,0x00,0x28,0x00,0x03,0x00,0x00,0x00,0x00,0x00};//last is ip   ,0xac,0x10,0x2a,0xd8//#######119Byte
u_char tailer_eap_http_resp2[]={0x73, 0x40, 0xea, 0x12, 0x00 ,
0x60, 0x04, 0xff, 0x73, 0x50, 0x02, 0xff, 0x73, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x04, 0xff, 0x73,
0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00};//########37Byte
/***************#大致含义为要求检测antiarp.exe等进程，然后发送报文已经检测到#*****************/
/* #####   GLOBLE VAR DEFINITIONS   ######################### */
/*-----------------------------------------------------------------------------
 *  程序的主控制变量
 *-----------------------------------------------------------------------------*/
int         lockfile;
char        errbuf[PCAP_ERRBUF_SIZE];  /* error buffer */
enum STATE  state = READY;                     /* program state */
pcap_t      *handle = NULL;			   /* packet capture handle */
u_char      muticast_mac[] =            /* 802.1x的认证服务器多播地址 */
                        {0x01, 0x80, 0xc2, 0x00, 0x00, 0x03};


/* #####   GLOBLE VAR DEFINITIONS   ###################
 *-----------------------------------------------------------------------------
 *  用户信息的赋值变量，由init_argument函数初始化
 *-----------------------------------------------------------------------------*/
int         background = 0;            /* 后台运行标记  */     
char        *dev = NULL;               /* 连接的设备名 */
char        *username = NULL;          
char        *password = NULL;
int         exit_flag = 0;
int         debug_on = 0;

/* #####   GLOBLE VAR DEFINITIONS   ######################### 
 *-----------------------------------------------------------------------------
 *  报文相关信息变量，由init_info 、init_device函数初始化。
 *-----------------------------------------------------------------------------*/
int         username_length;
int         password_length;
u_int       local_ip = 0;
u_char      local_mac[ETHER_ADDR_LEN]; /* MAC地址 */
char        dev_if_name[64];

/* #####   TYPE DEFINITIONS   ######################### */
/*-----------------------------------------------------------------------------
 *  报文缓冲区，由init_frame函数初始化。
 *-----------------------------------------------------------------------------*/
u_char      eapol_start[64];            /* EAPOL START报文 */
u_char      eapol_logoff[64];           /* EAPOL LogOff报文 */
u_char      eapol_keepalive[64];
u_char      eap_response_ident[128]; /* EAP RESPON/IDENTITY报文 */
u_char      eap_response_md5ch[128]; /* EAP RESPON/MD5 报文 */
/*********#新增加的HTTP_DIGEST的回应部分报文#***************/
u_char      eap_response_http1[146]; /** EAP_RESPONSE_HTTP1_DIGEST报文**/ //#####add By An For Haut [Tavakoli]
u_char      eap_response_http2[64];  /** EAP_RESPONSE_HTTP2_DIGEST报文**/ //#####add By An For Haut [Tavakoli]
int         http_num=0;    /**0 or1 @:此参数作为分辨http1或者2的计数器**/
/*********#新增加的HTTP_DIGEST的回应部分报文#***************/

//u_int       live_count = 0;             /* KEEP ALIVE 报文的计数值 */
//pid_t       current_pid = 0;            /* 记录后台进程的pid */

// debug function
void 
print_hex(const uint8_t *array, int count)
{
    int i;
    for(i = 0; i < count; i++){
        if ( !(i % 16))
            printf ("\n");
        printf("%02x ", array[i]);
    }
    printf("\n");
}

int 
code_convert(char *from_charset, char *to_charset,
             char *inbuf, size_t inlen, char *outbuf, size_t outlen)
{
    iconv_t cd;
    char **pin = &inbuf;
    char **pout = &outbuf;

    cd = iconv_open(to_charset,from_charset);

    if (cd==0) 
      return -1;
    memset(outbuf,0,outlen);

    if (iconv(cd, pin, &inlen, pout, &outlen)==-1) 
      return -1;
    iconv_close(cd);
    return 0;
}

void 
print_server_info (const u_char *str)
{
    if (!(str[0] == 0x2f && str[1] == 0xfc)) 
        return;

    char info_str [1024] = {0};
    int length = str[2];
    if (code_convert ("gb2312", "utf-8", (char*)str + 3, length, info_str, 200) != 0){
        fprintf (stderr, "@@Error: Server info convert error.\n");
        return;
    }
    fprintf (stdout, "&&Server Info: %s\n", info_str);
}

void
show_usage()
{
    printf( "\n"
            "ZlevoClient %s \n"
            "\t  -- Supllicant for Lenovo 802.1x Authentication.\n"
            "\n"
            "  Usage:\n"
            "\tRun under root privilege, usually by `sudo', with your \n"
            "\taccount info in arguments:\n\n"
            "\t-u, --username           Your username.\n"
            "\t-p, --password           Your password.\n"
            "\n"
            "  Optional Arguments:\n\n"
            "\t--device              Specify which device to use.\n"
            "\t                      Default is usually eth0.\n\n"

            "\t-b, --background      Program fork to background after authentication.\n\n"
            "\t-l                    Tell the process to Logoff.\n\n"
            "\t--debug               Show debug message.\n\n"
            "\t-h, --help            Show this help.\n\n"
            "\n"
            "  About ZlevoClient:\n\n"
            "\tThis program is a supplicat program compatible for LENOVO ,\n"
            "\t802.1x EAPOL protocol, which was used for  Internet control.\n"

            "\tZlevoClient is a software developed individually, with NO any rela-\n"
            "\tiontship with Lenovo company.\n\n\n"
            
            "\tAnother PT work. Blog: http://apt-blog.co.cc\n"
            "\t\t\t\t\t\t\t\t2009.05.24\n",
            LENOVO_VER);
}

/* 
 * ===  FUNCTION  ======================================================================
 *         Name:  get_md5_digest
 *  Description:  calcuate for md5 digest
 * =====================================================================================
 */
char* 
get_md5_digest(const char* str, size_t len)
{
    static md5_byte_t digest[16];
	md5_state_t state;
	md5_init(&state);
	md5_append(&state, (const md5_byte_t *)str, len);
	md5_finish(&state, digest);

    return (char*)digest;
}


enum EAPType 
get_eap_type(const struct sniff_eap_header *eap_header) 
{
     if(1 == debug_on){//增加测试内容
        fprintf (stdout, "&&IMPORTANT: Current Package : eap_t:      %02x\n"
                    "                               eap_id: %02x\n"
                    "                               eap_op:     %02x\n", 
                    eap_header->eap_t, eap_header->eap_id,
                    eap_header->eap_op);
      }//增加测试内容
    switch (eap_header->eap_t){
        case 0x01:
            if (eap_header->eap_op == 0x01)
                    return EAP_REQUEST_IDENTITY;
            if (eap_header->eap_op == 0x04)
                    return EAP_REQUETS_MD5_CHALLENGE;
/******************此处为判断eap类型为0x26即EAP_HTTP_DIGEST******************/
            if (eap_header->eap_op == 0x26)////add By An For Haut
             {
                if(http_num == 0){
                     http_num=1;
                     return EAP_REQUEST_HTTP_DIGEST1;////add By An For Haut
                     }	
                     else if(http_num == 1){
                     http_num=0;
                     return EAP_REQUEST_HTTP_DIGEST2;////add By An For Haut
                     }
            }//end if(eap_header->eap_op == 0x26)
/******************此处为判断eap类型为0x26即EAP_HTTP_DIGEST******************/
            break;
        case 0x03:
        //    if (eap_header->eap_id == 0x02)
            return EAP_SUCCESS;
            break;
        case 0x04:
            return EAP_FAILURE;
    }
    fprintf (stderr, "&&IMPORTANT: Unknown Package : eap_t:      %02x\n"
                    "                               eap_id: %02x\n"
                    "                               eap_op:     %02x\n", 
                    eap_header->eap_t, eap_header->eap_id,
                    eap_header->eap_op);
    return ERROR;
}

void 
action_by_eap_type(enum EAPType pType, 
                        const struct sniff_eap_header *header) {
//    printf("PackType: %d\n", pType);
    switch(pType){
        case EAP_SUCCESS:
            state = ONLINE;
            fprintf(stdout, ">>Protocol: EAP_SUCCESS\n");
            fprintf(stdout, "&&Info: Authorized Access to Network. \n");
            if (background){
                background = 0;         /* 防止以后误触发 */
                daemon_init ();  /* fork至后台，主程序退出 */
            }

            /* Set alarm to send keep alive packet */
            alarm(KEEP_ALIVE_TIME);
            break;
        case EAP_FAILURE:
            if (state == READY) {
                fprintf(stdout, ">>Protocol: Init Logoff Signal\n");
                return;
            }
            state = READY;
            fprintf(stdout, ">>Protocol: EAP_FAILURE\n");
            if(state == ONLINE){
                fprintf(stdout, "&&Info: SERVER Forced Logoff\n");
            }
            if (state == STARTED){
                fprintf(stdout, "&&Info: Invalid Username or Client info mismatch.\n");
            }
            if (state == ID_AUTHED){
                fprintf(stdout, "&&Info: Invalid Password.\n");
            }
            print_server_info (header->eap_info_tailer);
            pcap_breakloop (handle);
            break;
        case EAP_REQUEST_IDENTITY:
            if (state == STARTED){
                fprintf(stdout, ">>Protocol: REQUEST EAP-Identity\n");
            }
            memset (eap_response_ident + 14 + 5, header->eap_id, 1);
            send_eap_packet(EAP_RESPONSE_IDENTITY);
            break;
        case EAP_REQUETS_MD5_CHALLENGE:
            state = ID_AUTHED;
            fprintf(stdout, ">>Protocol: REQUEST MD5-Challenge(PASSWORD)\n");
            fill_password_md5((u_char*)header->eap_info_tailer, 
                                        header->eap_id);
            send_eap_packet(EAP_RESPONSE_MD5_CHALLENGE);
            break;
/****此处为判断eap类型为0x26即EAP_REQUEST_HTTP_DIGEST的两个服务器发来的报文,然后填充id,发送相应报文****/
        case EAP_REQUEST_HTTP_DIGEST1:
            fprintf(stdout, ">>Protocol: EAP_REQUETS_HTTP_DIGEST-1    ///add by An For haut test!\n");
	    memset (eap_response_http1 + 14 + 5, header->eap_id, 1);/**146Byte**/
            send_eap_packet(EAP_RESPONSE_HTTP_DIGEST1);
            break;
        case EAP_REQUEST_HTTP_DIGEST2:
            fprintf(stdout, ">>Protocol: EAP_REQUETS_HTTP_DIGEST-2    ///add by An For haut test!\n");
	    memset (eap_response_http2 + 14 + 5, header->eap_id, 1);/**64Byte**/
            send_eap_packet(EAP_RESPONSE_HTTP_DIGEST2);
            break;
/****此处为判断eap类型为0x26即EAP_REQUEST_HTTP_DIGEST的两个服务器发来的报文,然后填充id,发送相应报文****/
	 
        default:
            return;
    }
}

void 
send_eap_packet(enum EAPType send_type)
{
    u_char *frame_data;
    int     frame_length = 0;
    switch(send_type){
        case EAPOL_START:
            state = STARTED;
            frame_data= eapol_start;
            frame_length = 64;
            fprintf(stdout, ">>Protocol: SEND EAPOL-Start\n");
            break;
        case EAPOL_LOGOFF:
            state = READY;
            frame_data = eapol_logoff;
            frame_length = 64;
            fprintf(stdout, ">>Protocol: SEND EAPOL-Logoff\n");
            break;
        case EAP_RESPONSE_IDENTITY:
            frame_data = eap_response_ident;
            frame_length = 54 + username_length;
            fprintf(stdout, ">>Protocol: SEND EAP-Response/Identity\n");
            break;
        case EAP_RESPONSE_MD5_CHALLENGE:
            frame_data = eap_response_md5ch;
            frame_length = 40 + username_length + 14;
            fprintf(stdout, ">>Protocol: SEND EAP-Response/Md5-Challenge\n");
            break;
        case EAP_RESPONSE_IDENTITY_KEEP_ALIVE:
            frame_data = eapol_keepalive;
            frame_length = 64;
            fprintf(stdout, ">>Protocol: SEND EAPOL Keep Alive\n");
            break;
/************发送EAP_HTTP_DIGEST的两种resonse类型报文************/
        case EAP_RESPONSE_HTTP_DIGEST1:
	    frame_data = eap_response_http1;
            frame_length = 146;
            fprintf(stdout, ">>Protocol: SEND EAP_RESPONSE_HTTP_DIGEST-1  //add by An For haut test!\n");
            break;
	case EAP_RESPONSE_HTTP_DIGEST2:
	    frame_data = eap_response_http2;
            frame_length = 64;
            fprintf(stdout, ">>Protocol: SEND EAP_RESPONSE_HTTP_DIGEST-2  //add by An For haut test!\n");
            break;
/************发送EAP_HTTP_DIGEST的两种resonse类型报文************/
        default:
            fprintf(stderr,"&&IMPORTANT: Wrong Send Request Type.%02x\n", send_type);
            return;
    }
    if (debug_on){
        printf ("@@DEBUG: Sent Frame Data:\n");
        print_hex (frame_data, frame_length);
    }
    if (pcap_sendpacket(handle, frame_data, frame_length) != 0)
    {
        fprintf(stderr,"&&IMPORTANT: Error Sending the packet: %s\n", pcap_geterr(handle));
        return;
    }
}

/* Callback function for pcap.  */
void
get_packet(u_char *args, const struct pcap_pkthdr *header, 
    const u_char *packet)
{
	/* declare pointers to packet headers */
	const struct sniff_ethernet *ethernet;  /* The ethernet header [1] */
    const struct sniff_eap_header *eap_header;

    ethernet = (struct sniff_ethernet*)(packet);
    eap_header = (struct sniff_eap_header *)(packet + SIZE_ETHERNET);

    if (debug_on){
        printf ("@@DEBUG: Packet Caputre Data:\n");
        print_hex (packet, 64);
    }

    enum EAPType p_type = get_eap_type(eap_header);
    action_by_eap_type(p_type, eap_header);

    return;
}

void 
init_frames()
{
    int data_index;

    /*****  EAPOL Header  *******/
    u_char eapol_header[SIZE_ETHERNET];
    data_index = 0;
    u_short eapol_t = htons (0x888e);
    memcpy (eapol_header + data_index, muticast_mac, 6); /* dst addr. muticast */
    data_index += 6;
    memcpy (eapol_header + data_index, local_mac, 6);    /* src addr. local mac */
    data_index += 6;
    memcpy (eapol_header + data_index, &eapol_t, 2);    /*  frame type, 0x888e*/

    /**** EAPol START ****/
    u_char start_data[] = {0x01, 0x01, 0x00, 0x00};
    memset (eapol_start, 0xcc, 64);
    memcpy (eapol_start, eapol_header, 14);
    memcpy (eapol_start + 14, start_data, 4);
    memcpy (eapol_start + 14 + 4, talier_eapol_start, 6);

//    print_hex(eapol_start, sizeof(eapol_start));
    /****EAPol LOGOFF ****/
    u_char logoff_data[4] = {0x01, 0x02, 0x00, 0x00};
    memset (eapol_logoff, 0xcc, 64);
    memcpy (eapol_logoff, eapol_header, 14);
    memcpy (eapol_logoff + 14, logoff_data, 4);
    memcpy (eapol_logoff + 14 + 4, talier_eapol_start, 4);

//    print_hex(eapol_logoff, sizeof(eapol_logoff));

    /****EAPol Keep alive ****/
    u_char keep_data[4] = {0x01, 0xfc, 0x00, 0x0c};
    memset (eapol_keepalive, 0xcc, 64);
    memcpy (eapol_keepalive, eapol_header, 14);
    memcpy (eapol_keepalive + 14, keep_data, 4);
    memset (eapol_keepalive + 18, 0, 8);
    memcpy (eapol_keepalive + 26, &local_ip, 4);
    
//    print_hex(eapol_keepalive, sizeof(eapol_keepalive));

    /* EAP RESPONSE IDENTITY */
    u_char eap_resp_iden_head[9] = {0x01, 0x00, 
                                    0x00, 5 + username_length,  /* eapol_length */
                                    0x02, 0x00, 
                                    0x00, 5 + username_length,       /* eap_length */
                                    0x01};
    
//    eap_response_ident = malloc (54 + username_length);
    memset(eap_response_ident, 0xcc, 54 + username_length);

    data_index = 0;
    memcpy (eap_response_ident + data_index, eapol_header, 14);
    data_index += 14;
    memcpy (eap_response_ident + data_index, eap_resp_iden_head, 9);
    data_index += 9;
    memcpy (eap_response_ident + data_index, username, username_length);

//    print_hex(eap_response_ident, 54 + username_length);
    /** EAP RESPONSE MD5 Challenge **/
    u_char eap_resp_md5_head[10] = {0x01, 0x00, 
                                   0x00, 6 + 16 + username_length, /* eapol-length */
                                   0x02, 
                                   0x00, /* id to be set */
                                   0x00, 6 + 16 + username_length, /* eap-length */
                                   0x04, 0x10};
//    eap_response_md5ch = malloc (14 + 4 + 6 + 16 + username_length + 14);
//    memset(eap_response_md5ch, 0xcc, 14 + 4 + 6 + 16 + username_length + 14);

    data_index = 0;
    memcpy (eap_response_md5ch + data_index, eapol_header, 14);
    data_index += 14;
    memcpy (eap_response_md5ch + data_index, eap_resp_md5_head, 10);
    data_index += 26;// 剩余16位在收到REQ/MD5报文后由fill_password_md5填充 
    memcpy (eap_response_md5ch + data_index, username, username_length);
    data_index += username_length;
    memcpy (eap_response_md5ch + data_index, &local_ip, 4);
    data_index += 4;
    memcpy (eap_response_md5ch + data_index, talier_eap_md5_resp, 9);

//    print_hex(eap_response_md5ch, 14 + 4 + 6 + 16 + username_length + 14);
/************初始化发送EAP_HTTP_DIGEST的两种resonse类型报文************/

	/* EAP Resonse Http -1 */
    u_char eap_resp_http1_head[9] = {0x01,0x00,
                                     0x00,0x7c,
                                     0x02,
                                     0x00,		//id to be set (http1)
                                     0x00,0x7c,		//0x7c = 124 = length of (1+1+2+1)+tavakoli;tavakoli=119Byte
                                     0x26/* type=38 */};//toatol=146Byte
    data_index = 0;
    memcpy (eap_response_http1 + data_index, eapol_header, 14);
    data_index += 14;
    memcpy (eap_response_http1 + data_index, eap_resp_http1_head, 9);
    data_index += 9;
    memcpy (eap_response_http1 + data_index, tailer_eap_http_resp1, 119);
    data_index += 119;
    memcpy (eap_response_http1 + data_index, &local_ip, 4);

	/* EAP Resonse Http -2 */
    u_char eap_resp_http2_head[9] = {0x01,0x00,
                                     0x00,0x09,		//0x09 == length of(1+1+2+1)+IP
                                     0x02,
                                     0x00,		//id to be set (http2)
                                     0x00,0x05,
                                     0x26/* type=38 */};//toatol=64Byte
     data_index = 0;
    memcpy (eap_response_http2 + data_index, eapol_header, 14);
    data_index += 14;
    memcpy (eap_response_http2 + data_index, eap_resp_http2_head, 9);
    data_index += 9;
    memcpy (eap_response_http2 + data_index, &local_ip, 4);
     data_index += 4;
    memcpy (eap_response_http2 + data_index, tailer_eap_http_resp2, 37);

/************初始化发送EAP_HTTP_DIGEST的两种resonse类型报文************/
}

void 
fill_password_md5(u_char *attach_key, u_int id)
{
    char *psw_key = malloc(1 + password_length + 16);
    char *md5;
    psw_key[0] = id;
    memcpy (psw_key + 1, password, password_length);
    memcpy (psw_key + 1 + password_length, attach_key, 16);

    if (debug_on){
        printf("@@DEBUG: MD5-Attach-KEY:\n");
        print_hex ((u_char*)psw_key, 1 + password_length + 16);
    }

    md5 = get_md5_digest(psw_key, 1 + password_length + 16);

    memset (eap_response_md5ch + 14 + 5, id, 1);
    memcpy (eap_response_md5ch + 14 + 10, md5, 16);

    free (psw_key);
}

void init_info()
{
    if(username == NULL || password == NULL){
        fprintf (stderr,"Error: NO Username or Password promoted.\n"
                        "Try zlevoclient --help for usage.\n");
        exit(EXIT_FAILURE);
    }
    username_length = strlen(username);
    password_length = strlen(password);

}

/* 
 * ===  FUNCTION  ======================================================================
 *         Name:  init_device
 *  Description:  初始化设备。主要是找到打开网卡、获取网卡MAC、IP，
 *  同时设置pcap的初始化工作句柄。
 * =====================================================================================
 */
void init_device()
{
    struct          bpf_program fp;			/* compiled filter program (expression) */
    char            filter_exp[51];         /* filter expression [3] */
    pcap_if_t       *alldevs;
    pcap_addr_t     *addrs;

	/* Retrieve the device list */
	if(pcap_findalldevs(&alldevs, errbuf) == -1)
	{
		fprintf(stderr,"Error in pcap_findalldevs: %s\n", errbuf);
		exit(1);
	}

    /* 使用第一块设备 */
    if(dev == NULL) {
        dev = alldevs->name;
        strcpy (dev_if_name, dev);
    }

	if (dev == NULL) {
		fprintf(stderr, "Couldn't find default device: %s\n",
			errbuf);
		exit(EXIT_FAILURE);
    }
	
	/* open capture device */
	handle = pcap_open_live(dev, SNAP_LEN, 1, 1000, errbuf);

	if (handle == NULL) {
		fprintf(stderr, "Couldn't open device %s: %s\n", dev, errbuf);
		exit(EXIT_FAILURE);
	}

	/* make sure we're capturing on an Ethernet device [2] */
	if (pcap_datalink(handle) != DLT_EN10MB) {
		fprintf(stderr, "%s is not an Ethernet\n", dev);
		exit(EXIT_FAILURE);
	}

    /* Get IP ADDR and MASK */
    for (addrs = alldevs->addresses; addrs; addrs=addrs->next) {
        if (addrs->addr->sa_family == AF_INET) {
            local_ip = ((struct sockaddr_in *)addrs->addr)->sin_addr.s_addr;
        }
    }

#ifdef __linux
    /* get device basic infomation */
    struct ifreq ifr;
    int sock;
    if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
    strcpy(ifr.ifr_name, dev);

    //获得网卡Mac
    if(ioctl(sock, SIOCGIFHWADDR, &ifr) < 0)
    {
        perror("ioctl");
        exit(EXIT_FAILURE);
    }
    memcpy(local_mac, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
#else
    if (bsd_get_mac (dev, local_mac) != 0) {
		fprintf(stderr, "FATIL: Fail getting BSD/MACOS Mac Address.\n");
		exit(EXIT_FAILURE);
    }
#endif

    /* construct the filter string */
    sprintf(filter_exp, "ether dst %02x:%02x:%02x:%02x:%02x:%02x"
                        " and ether proto 0x888e", 
                        local_mac[0], local_mac[1],
                        local_mac[2], local_mac[3],
                        local_mac[4], local_mac[5]);

	/* compile the filter expression */
	if (pcap_compile(handle, &fp, filter_exp, 0, 0) == -1) {
		fprintf(stderr, "Couldn't parse filter %s: %s\n",
		    filter_exp, pcap_geterr(handle));
		exit(EXIT_FAILURE);
	}

	/* apply the compiled filter */
	if (pcap_setfilter(handle, &fp) == -1) {
		fprintf(stderr, "Couldn't install filter %s: %s\n",
		    filter_exp, pcap_geterr(handle));
		exit(EXIT_FAILURE);
	}
    pcap_freecode(&fp);
    pcap_freealldevs(alldevs);
}

static void
signal_interrupted (int signo)
{
    fprintf(stdout,"\n&&Info: USER Interrupted. \n");
    send_eap_packet(EAPOL_LOGOFF);
    pcap_breakloop (handle);
    pcap_close (handle);
    exit (EXIT_FAILURE);
}

void init_arguments(int argc, char **argv)
{
    /* Option struct for progrm run arguments */
    struct option long_options[] =
        {
        {"help",        no_argument,        0,              'h'},
        {"background",  no_argument,        &background,    1},
        {"device",      required_argument,  0,              2},
        {"username",    required_argument,  0,              'u'},
        {"password",    required_argument,  0,              'p'},
        {"debug",       no_argument,        &debug_on,      'd'},
        {0, 0, 0, 0}
        };

    int c;
    while (1) {

        /* getopt_long stores the option index here. */
        int option_index = 0;
        c = getopt_long (argc, argv, "u:p:hbl",
                        long_options, &option_index);
        if (c == -1)
            break;
        switch (c) {
            case 0:
               break;
            case 'b':
                background = 1;
                break;
            case 2:
                dev = optarg;
                break;
            case 'u':
                username = optarg;
                break;
            case 'p':
                password = optarg;
                break;
            case 'h':
                show_usage();
                exit(EXIT_SUCCESS);
                break;
            case 'l':
                exit_flag = 1;
                break;
            case '?':
                if (optopt == 'u' || optopt == 'p'|| optopt == 'g'|| optopt == 'd')
                    fprintf (stderr, "Option -%c requires an argument.\n", optopt);
                exit(EXIT_FAILURE);
                break;
            default:
                fprintf (stderr,"Unknown option character `\\x%x'.\n", c);
                exit(EXIT_FAILURE);
        }
    }    
}

void keep_alive()
{
    send_eap_packet (EAP_RESPONSE_IDENTITY_KEEP_ALIVE);
    alarm(KEEP_ALIVE_TIME);
}

void
flock_reg ()
{
    char buf[16];
    struct flock fl;
    fl.l_start = 0;
    fl.l_whence = SEEK_SET;
    fl.l_len = 0;
    fl.l_type = F_WRLCK;
    fl.l_pid = getpid();
 
    //阻塞式的加锁
    if (fcntl (lockfile, F_SETLKW, &fl) < 0){
        perror ("fcntl_reg");
        exit(EXIT_FAILURE);
    }
 
    //把pid写入锁文件
    assert (0 == ftruncate (lockfile, 0) );    
    sprintf (buf, "%ld", (long)getpid());
    assert (-1 != write (lockfile, buf, strlen(buf) + 1));
}


void
daemon_init(void)
{
	pid_t	pid;
    int     fd0;

	if ( (pid = fork()) < 0)
	    perror ("Fork");
	else if (pid != 0) {
        fprintf(stdout, "&&Info: Forked background with PID: [%d]\n\n", pid);
		exit(EXIT_SUCCESS);
    }
	setsid();		/* become session leader */
	assert (0 == chdir("/tmp"));		/* change working directory */
	umask(0);		/* clear our file mode creation mask */
    flock_reg ();

    fd0 = open ("/dev/null", O_RDWR);
    dup2 (fd0, STDIN_FILENO);
    dup2 (fd0, STDERR_FILENO);
    dup2 (fd0, STDOUT_FILENO);
    close (fd0);
}


int 
program_running_check()
{
    struct flock fl;
    fl.l_start = 0;
    fl.l_whence = SEEK_SET;
    fl.l_len = 0;
    fl.l_type = F_WRLCK;
 
    //尝试获得文件锁
    if (fcntl (lockfile, F_GETLK, &fl) < 0){
        perror ("fcntl_get");
        exit(EXIT_FAILURE);
    }

    if (exit_flag) {
        if (fl.l_type != F_UNLCK) {
            if ( kill (fl.l_pid, SIGINT) == -1 )
                perror("kill");
            fprintf (stdout, "&&Info: Kill Signal Sent to PID %d.\n", fl.l_pid);
        }
        else 
            fprintf (stderr, "&&Info: NO zLenovoClient Running.\n");
        exit (EXIT_FAILURE);
    }


    //没有锁，则给文件加锁，否则返回锁着文件的进程pid
    if (fl.l_type == F_UNLCK) {
        flock_reg ();
        return 0;
    }

    return fl.l_pid;
}


int main(int argc, char **argv)
{   
    init_arguments (argc, argv);

    //打开锁文件
    lockfile = open (LOCKFILE, O_RDWR | O_CREAT , LOCKMODE);
    if (lockfile < 0){
        perror ("Lockfile");
        exit(1);
    }

    int ins_pid;
    if ( (ins_pid = program_running_check ()) ) {
        fprintf(stderr,"@@ERROR: ZLevoClient Already "
                            "Running with PID %d\n", ins_pid);
        exit(EXIT_FAILURE);
    }

    init_info();
    init_device();
    init_frames ();

    signal (SIGINT, signal_interrupted);
    signal (SIGTERM, signal_interrupted);
    signal (SIGALRM, keep_alive);

    printf("######## Lenovo Client ver. %s #########\n", LENOVO_VER);
    printf("Device:     %s\n", dev_if_name);
    printf("MAC:        %02x:%02x:%02x:%02x:%02x:%02x\n",
                        local_mac[0],local_mac[1],local_mac[2],
                        local_mac[3],local_mac[4],local_mac[5]);
    printf("IP:         %s\n", inet_ntoa(*(struct in_addr*)&local_ip));
    printf("########################################\n");

//    send_eap_packet (EAPOL_LOGOFF);
    send_eap_packet (EAPOL_START);

	pcap_loop (handle, -1, get_packet, NULL);   /* main loop */

    send_eap_packet (EAPOL_LOGOFF);

	pcap_close (handle);
//    free (eap_response_ident);
//    free (eap_response_md5ch);
    return EXIT_SUCCESS;
}

#ifndef __linux
int bsd_get_mac(const char ifname[], uint8_t eth_addr[])
{
    struct ifreq *ifrp;
    struct ifconf ifc;
    char buffer[720];
    int socketfd,error,len,space=0;
    ifc.ifc_len=sizeof(buffer);
    len=ifc.ifc_len;
    ifc.ifc_buf=buffer;

    socketfd=socket(AF_INET,SOCK_DGRAM,0);

    if((error=ioctl(socketfd,SIOCGIFCONF,&ifc))<0)
    {
        perror("ioctl faild");
        exit(1);
    }
    if(ifc.ifc_len<=len)
    {
        ifrp=ifc.ifc_req;
        do
        {
            struct sockaddr *sa=&ifrp->ifr_addr;
            
            if(((struct sockaddr_dl *)sa)->sdl_type==IFT_ETHER) {
                if (strcmp(ifname, ifrp->ifr_name) == 0){
                    memcpy (eth_addr, LLADDR((struct sockaddr_dl *)&ifrp->ifr_addr), 6);
                    return 0;
                }
            }
            ifrp=(struct ifreq*)(sa->sa_len+(caddr_t)&ifrp->ifr_addr);
            space+=(int)sa->sa_len+sizeof(ifrp->ifr_name);
        }
        while(space<ifc.ifc_len);
    }
    return 1;
}
#endif
