/*
* CMPT464 Assignment 2
* Sage Jurr
* Selena Lovelace
* Charuni Liyanage
* Distributed Database
*/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "sysio.h"
#include "serf.h"
#include "ser.h"

#include "phys_cc1350.h"
#include "plug_null.h"
#include "tcv.h"


#define CC1350_BUF_SZ 250
#define DISC_REQ 0
#define DISC_RES 1

#define CREATE_REC 2
#define DELETE_REC 3
#define GET_REC 4
#define RES_REC 5

#define MAX_RECORDS 40
#define RECORD_LENGTH 20

/*********************** Global Variables and Structs ************************/

/* session descriptor for the single VNETI session */
int sfd;

/* IDs etc for messages */
int group_id = 3;
int node_id = 1;
char rec_id[4];
char neighbours[10];

// Packet Struct
struct pkt_struct {
	byte group_id;
	byte type;
	byte request_num;
	byte pad;
	byte sender_id;
	byte receiver_id;
	byte record_status;
	char message[20];
};

// Database structs
typedef struct{
// when create request recieved, the sender of that request is the owner of the record
// retrieve sends record back to requester
// delete request we would delete the record requested
	time_t timeStamp;
	int ownerID;
	char payload[RECORD_LENGTH];
}record;

record database[MAX_RECORDS];
// keeps track of record entries

// For tracking database entries
int entries;
int currRec= 0;
int curr_store = 0;

// global
struct pkt_struct * disc_req;
struct pkt_struct * disc_res;
struct pkt_struct * create_req;
struct pkt_struct * delete_req;
struct pkt_struct * send_req;


/********************* Receiver FSM, concurrent to root **********************/

fsm receiver {
	address packet; //received packets

	address packet_res; //to build responses

	struct pkt_struct * rcv_pkt;

	// Get packet
	state Receiving:
		packet = tcv_rnp(Receiving,sfd);

	// If packet properly received
	state OK:
		// Cast packet into readable structure
		rcv_pkt = (struct pkt_struct *)(packet+1);

		// Check if correct group ID, return / ignore if wrong
			if (rcv_pkt->group_id != group_id){
			proceed Receiving;
		}

		// Check if correct node ID (itself or 0), return /ignore if wrong
			if ((rcv_pkt->receiver_id != node_id) && (rcv_pkt->receiver_id != 0)){
			proceed Receiving;
		}

		// Discovery Request
		if (rcv_pkt->type == 0){
		// Build Response
		disc_res = (struct pkt_struct *)umalloc(sizeof(struct pkt_struct));
		disc_res->group_id = group_id;
		disc_res->type = DISC_RES;
		disc_res->request_num = rcv_pkt->request_num;
		disc_res->pad = 0;
		disc_res->sender_id = node_id;
		disc_res->receiver_id = 0;
		disc_res->record_status = 0;

		// Finish building discovery response packet and send off
		packet_res = tcv_wnp(OK, sfd, 30);
		packet_res[0] = 0;
		char *p = (char *)(packet_res+1);
		*p = disc_res->group_id; p++;
		*p = disc_res->type; p++;
		*p = disc_res->request_num; p++;
		*p = disc_res->pad; p++;
		*p = disc_res->sender_id; p++;
		*p = disc_res->receiver_id; p++;
		*p = disc_res->record_status; p++;
		strcat(p, disc_res->message);
		tcv_endp(packet_res);
		tcv_endp(packet);
		ufree(rcv_pkt);
		ufree(disc_res); // Free up malloc'd space for sent packet
		memset(neighbours, 0, sizeof(neighbours));; // Reset neighbours array 
		proceed Receiving;
		}

		// Discovery Response
		else if (rcv_pkt->type == DISC_RES){
			// Record response
			int neighs = strlen(neighbours);
			neighbours[neighs] = rcv_pkt->sender_id;
			ufree(rcv_pkt);
			tcv_endp(packet);
			proceed Receiving;
		}


		// if create record on neighbour is received
		else if (rcv_pkt->type == CREATE_REC){
		proceed createRecord;
		}
		// if destroy record on neighbour is received
		else if(rcv_pkt->type == DELETE_REC){
		proceed deleteRecord;
		} 
		else if(rcv_pkt->type == GET_REC){
		proceed getRecord;
		} 
		else if(rcv_pkt->type == RES_REC){
		proceed responseRecord;
		} 

		tcv_endp(packet);
		proceed Receiving;

	// Handles Creating Records
	state createRecord:
		if (entries >= MAX_RECORDS){
			ser_out(createRecord, "\r\n Maximum records reached");
		}

		else {
			database[entries].ownerID = rcv_pkt->sender_id;
			strncpy(database[entries].payload, rcv_pkt->message, 20); 
			database[entries].timeStamp = time(NULL);
			entries++;
			curr_store++;
			ser_out(createRecord, "\r\n Data Saved");
		}    	

		// acknowledgement *would* go here
		tcv_endp(packet);
		proceed Receiving;

	// Handles deleting existing record
	state deleteRecord:

		int index = (int)(rcv_pkt->message[0]); // cast str int

		if (entries == 0){
			ser_out(deleteRecord, "\r\n No record to delete");
		}

		else if(index >= entries) {
			ser_out(deleteRecord, "\r\n Does not exist");
		}

		else{
			for (int i = index; i < entries; i++){

			database[i] = database[i+1]; // shift entries to delete
			}
			entries--;
			curr_store--;
			}

		// acknowledgement *would* go here
		tcv_endp(packet);
		proceed Receiving;

	// Handles Showing Records
	state getRecord:
		int index;
		index = (int)(rcv_pkt->message[0]); // cast str int		
		if (entries == 0){
			ser_out(getRecord, "\r\n No record in database");
		}else if (database[index].ownerID == NULL) {
			ser_out(getRecord, "\r\n Does not exist");
		}else{ 
			ser_outf(getRecord, "\r\n %s GOTTEEEE", database[index].payload); 
		}
		// acknowledgement *would* go here
		tcv_endp(packet);
		proceed Receiving;

	// Handles responses
	state responseRecord:
		if (entries == 0){
			ser_out(responseRecord, "\r\n No record in database");
		}else{ 
			ser_outf(responseRecord, "\r\n %s", rcv_pkt ->message); 
		}
		// acknowledgement *would* go here
		tcv_endp(packet);
		proceed Receiving;

		}

// Main FSM for sending packets
fsm root {
	char msg_string[20];

	int total_store = 40;
	address packet;    

	/*Initialization*/
	state INIT:
	phys_cc1350 (0, CC1350_BUF_SZ);

	tcv_plug(0, &plug_null);
	sfd = tcv_open(NONE, 0, 0);
	if (sfd < 0) {
		diag("unable to open TCV session");
		syserror(EASSERT, "no session");
	}

	tcv_control(sfd, PHYSOPT_ON, NULL);
	runfsm receiver;

/********************** User menu and selection states ***********************/
	state MENU:
		ser_outf (MENU,
		"\r\nGroup %d Device #%d (%d/%d records)\r\n"
		"(G)roup ID\r\n"
		"(N)ew device ID\r\n"
		"(F)ind neighbours\r\n"
		"(C)reate record on neighbour\r\n"
		"(D)elete record from neighbour\r\n"
		"(R)etrieve record from neighbour\r\n"
		"(S)how local records\r\n"
		"R(e)set local storage\r\n\r\n"
		"Selection: ",
		group_id, node_id, curr_store, total_store
		);

// User selection and redirection to correct state
	state SELECT:
		char cmd[4];
		ser_inf(SELECT, "%c", cmd);

		if ((cmd[0] == 'G') || (cmd[0] == 'g'))
			proceed CHANGE_GID_PROMPT;
		else if ((cmd[0] == 'N') || (cmd[0] == 'n'))
			proceed CHANGE_NID_PROMPT;
		else if ((cmd[0] == 'F') || (cmd[0] == 'f'))
			proceed FIND_PROTOCOL;
		else if ((cmd[0] == 'C') || (cmd[0] == 'c'))
			proceed PRINT_REC_ID;
		else if ((cmd[0] == 'D' || cmd[0] == 'd'))
			proceed PRINT_REC_ID2;
		else if ((cmd[0] == 'R') || (cmd[0] == 'r'))
			proceed MENU; // could not finish
 		else if ((cmd[0] == 'S') || (cmd[0] == 's'))
			proceed SHOW_RECORDS;
		else if ((cmd[0] == 'E') || (cmd[0] == 'e'))
			proceed RESET;
		else
			proceed INPUT_ERROR;
	
	// Bad user input
	state INPUT_ERROR:
		ser_out(INPUT_ERROR, "Invalid command\r\n");
		proceed MENU;

/********************** Change Group & Node ID States ************************/

	// Change Group ID states
	state CHANGE_GID_PROMPT:
		ser_out(CHANGE_GID_PROMPT, "New Group ID (1-16): ");

	// Parse user input for Group ID
	state CHANGE_GID:
		char temp_id[4];
		ser_inf(CHANGE_GID, "%d", temp_id);
		if ((temp_id[0] > 0) && (temp_id[0] < 17)){
			group_id = temp_id[0];
			proceed MENU;
		}
		else
			proceed CHANGE_GID_PROMPT;

	// Change Node ID states
	state CHANGE_NID_PROMPT:
		ser_out(CHANGE_NID_PROMPT, "New Node ID (1-25): ");

	// Parse user input for Node ID
	state CHANGE_NID:
		char temp_id[4];
		ser_inf(CHANGE_NID, "%d", temp_id);
		if ((temp_id[0] > 0) && (temp_id[0] < 26)){
			node_id = temp_id[0];
			proceed MENU;
		}
		else
		proceed CHANGE_NID_PROMPT;
	
/**************************** Find Protocol States ***************************/

	// Build Discovery Request Packet
	state FIND_PROTOCOL:
		disc_req = (struct pkt_struct *)umalloc(sizeof(struct pkt_struct));
		disc_req->group_id = group_id;
		disc_req->type = DISC_REQ;
		disc_req->request_num = rand() % 255; 
		disc_req->pad = 0;
		disc_req->sender_id = node_id;
		disc_req->receiver_id = 0;
		disc_req->record_status = 0;

	// Finish building discovery request packet and send off
	state FIND_SEND:
		packet = tcv_wnp(FIND_SEND, sfd, 30);
		packet[0] = 0;
		char *p = (char *)(packet+1);
		*p = disc_res->group_id; p++;
		*p = disc_res->type; p++;
		*p = disc_res->request_num; p++;
		*p = disc_res->pad; p++;
		*p = disc_res->sender_id; p++;
		*p = disc_res->receiver_id; p++;
		*p = disc_res->record_status; p++;
		strcat(p, disc_res->message);

		tcv_endp(packet);
		ufree(disc_req); // Free up malloc'd space for sent packet
		delay(3*1024, FIND_PRINT);
		release;

	// Print results
	state FIND_PRINT:
		if (strlen(neighbours) > 0){
			ser_outf(FIND_PRINT, "Neighbours: %s\r\n", neighbours);
			proceed MENU;
		}
		else{
			ser_out(FIND_PRINT, "No neighbours\r\n");
			proceed MENU;
		}
			
/**************************** Create Protocol States ***************************/
	state PRINT_REC_ID:
		ser_out(PRINT_REC_ID, "Send rec_id\r\n");
		proceed CREATE_RECORD;

	state CREATE_RECORD:
		create_req = (struct pkt_struct *)umalloc(sizeof(struct pkt_struct));
		create_req->group_id = group_id;
		create_req->type = CREATE_REC;
		create_req->request_num = rand() % 255; 
		create_req->pad = 0;
		create_req->sender_id = node_id;
		ser_inf(CREATE_RECORD, "%d", rec_id);
		create_req->receiver_id = rec_id[0];
		create_req->record_status = 0;

	state PRINT_REC_ID_MESSAGE:
		ser_out(PRINT_REC_ID_MESSAGE, "Message: \r\n");

	state PRINT_REC_ID_MESSAGE2:
		ser_in(PRINT_REC_ID_MESSAGE2, create_req->message, 20);

	// Finish building create request packet and send off
	state CREATE_SEND:
		packet = tcv_wnp(CREATE_SEND, sfd, 30);
		packet[0] = 0;
		char *p = (char *)(packet+1);
		*p = create_req->group_id; p++;
		*p = create_req->type; p++;
		*p = create_req->request_num; p++;
		*p = create_req->pad; p++;
		*p = create_req->sender_id; p++;
		*p = create_req->receiver_id; p++;
		*p = create_req->record_status; p++;
		// ser_in(CREATE_SEND, create_req->message, 20);

		strcat(p, create_req->message);

		tcv_endp(packet);
		ufree(create_req); // Free up malloc'd space for sent packet

		proceed MENU;


/**************************** Delete Protocol States ***************************/

	state PRINT_REC_ID2:
		ser_out(PRINT_REC_ID2, "Delete rec_id\r\n");
		proceed DELETE_RECORD;

	state DELETE_RECORD:
		delete_req = (struct pkt_struct *)umalloc(sizeof(struct pkt_struct));
		delete_req->group_id = group_id;
		delete_req->type = DELETE_REC;
		delete_req->request_num = rand() % 255; 
		delete_req->pad = 0;
		delete_req->sender_id = node_id;
		ser_inf(DELETE_RECORD, "%d", rec_id);
		delete_req->receiver_id = rec_id;
		delete_req->record_status = 0;

	state PRINT_DELETE_ID_MESSAGE:
		ser_out(PRINT_DELETE_ID_MESSAGE, "Message: \r\n");

	state PRINT_DELETE_ID_MESSAGE2:
		ser_in(PRINT_DELETE_ID_MESSAGE2, delete_req->message, 20);


	// Finish building create request packet and send off
	state DELETE_SEND:
		packet = tcv_wnp(DELETE_SEND, sfd, 30);
		packet[0] = 0;
		char *p = (char *)(packet+1);
		*p = delete_req->group_id; p++;
		*p = delete_req->type; p++;
		*p = delete_req->request_num; p++;
		*p = delete_req->pad; p++;
		*p = delete_req->sender_id; p++;
		*p = delete_req->receiver_id; p++;
		*p = delete_req->record_status; p++;

		strcat(p, delete_req->message);

		tcv_endp(packet);
		ufree(delete_req); // Free up malloc'd space for sent packet

		proceed MENU;

/**************************** Retrieve Protocol States ***************************/

// Couldn't finish, sorry

/**************************** Show Protocol States ***************************/

	state SHOW_RECORDS:
		currRec =0; 
		if (entries >0) {
			ser_out(SHOW_RECORDS,"Index\tTime Stamp\towner ID\tRecord Data\r\n");
		}else{
			ser_out(SHOW_RECORDS,"No Records to display\r\n");
			proceed MENU;
		}
		
	state SHOW_RECORD:
		if ( currRec < entries){
			ser_outf(SHOW_RECORD,"%d\t%d\t%d\t\t%s\r\n", currRec, 10, database[currRec].ownerID, database[currRec].payload);
		}else{
			proceed MENU;
		}
		currRec++;
		proceed SHOW_RECORD;


/**************************** Reset Protocol States ***************************/
	state RESET:
		entries =0; 
		curr_store = 0;
		proceed MENU;

}
