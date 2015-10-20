/* ************************************************************************
*   File: db.player.c                                     EmpireMUD 2.0b3 *
*  Usage: Database functions related to players and the player table      *
*                                                                         *
*  EmpireMUD code base by Paul Clarke, (C) 2000-2015                      *
*  All rights reserved.  See license.doc for complete information.        *
*                                                                         *
*  EmpireMUD based upon CircleMUD 3.0, bpl 17, by Jeremy Elson.           *
*  CircleMUD (C) 1993, 94 by the Trustees of the Johns Hopkins University *
*  CircleMUD is based on DikuMUD, Copyright (C) 1990, 1991.               *
************************************************************************ */

#include "conf.h"
#include "sysdep.h"

#include "structs.h"
#include "utils.h"
#include "db.h"
#include "comm.h"
#include "handler.h"
#include "skills.h"
#include "interpreter.h"
#include "dg_scripts.h"
#include "vnums.h"

/**
* Contents:
*   Getters
*   Account DB
*   Core Player DB
*   Helpers
*   Empire Player Management
*   Promo Codes
*/

#define LOG_BAD_TAG_WARNINGS  TRUE	// triggers syslogs for invalid pfile tags

// external vars
extern struct attribute_data_type attributes[NUM_ATTRIBUTES];
extern const char *condition_types[];
extern const char *custom_color_types[];
extern const char *extra_attribute_types[];
extern const char *genders[];
extern const struct material_data materials[NUM_MATERIALS];
extern const char *pool_types[];
extern int top_account_id;
extern int top_idnum;

// external funcs
ACMD(do_slash_channel);
void update_class(char_data *ch);

// local protos
void clear_player(char_data *ch);
static bool member_is_timed_out(time_t created, time_t last_login, double played_hours);
char_data *read_player_from_file(FILE *fl, char *name);
int sort_players_by_idnum(player_index_data *a, player_index_data *b);
int sort_players_by_name(player_index_data *a, player_index_data *b);
void write_player_to_file(FILE *fl, char_data *ch);


 //////////////////////////////////////////////////////////////////////////////
//// GETTERS /////////////////////////////////////////////////////////////////

/**
* This has the same purpose as get_player_vis_or_file, but won't screw anything
* up if the target is online but invisible. You must call store_loaded_char()
* if is_file == TRUE, or the player won't be stored. If you do NOT wish to save
* the character, use free_char() instead.
*
* @param char *name The player name
* @param bool *is_file A place to store whether or not we loaded from file
* @return char_data *ch or NULL
*/
char_data *find_or_load_player(char *name, bool *is_file) {
	player_index_data *index;
	char_data *ch = NULL;
	
	*is_file = FALSE;
	
	if ((index = find_player_index_by_name(name))) {
		if (!(ch = is_playing(index->idnum))) {
			if ((ch = load_player(index->name))) {
				*is_file = TRUE;
			}
		}
	}

	return ch;
}


/**
* Look up a player's index entry by idnum.
*
* @param int idnum The idnum to look up.
* @return player_index_data* The player, if any (or NULL).
*/
player_index_data *find_player_index_by_idnum(int idnum) {
	player_index_data *plr;
	HASH_FIND(idnum_hh, player_table_by_idnum, &idnum, sizeof(int), plr);
	return plr;
}


/**
* Look up a player's index entry by name.
*
* @param char *name The name to look up.
* @return player_index_data* The player, if any (or NULL).
*/
player_index_data *find_player_index_by_name(char *name) {
	player_index_data *plr;
	HASH_FIND(name_hh, player_table_by_name, name, strlen(name), plr);
	return plr;
}


/**
* Finds a character who is sitting at a menu, for various functions that update
* all players and check which are in-game vs not. If a person is at a menu,
* then to safely update them you should change both their live data and saved
* data.
*
* @param int id A player id.
* @return char_data* A character in a menu state, or NULL if none.
*/
char_data *is_at_menu(int id) {
	descriptor_data *desc;
	
	for (desc = descriptor_list; desc; desc = desc->next) {
		if (STATE(desc) != CON_PLAYING && desc->character && !IS_NPC(desc->character) && GET_IDNUM(desc->character) == id) {
			return desc->character;
		}
	}
	
	return NULL;
}


/**
* @param int id A player id
* @return char_data* if the player is in the game, or NULL otherwise
*/
char_data *is_playing(int id) {
	char_data *ch;

	for (ch = character_list; ch; ch = ch->next) {
		if (!IS_NPC(ch) && GET_IDNUM(ch) == id) {
			return ch;
		}
	}
	return NULL;
}


 //////////////////////////////////////////////////////////////////////////////
//// ACCOUNT DB //////////////////////////////////////////////////////////////

/**
* Add an account to the hash table.
*
* @param account_data *acct The account to add.
*/
void add_account_to_table(account_data *acct) {
	int sort_accounts(account_data *a, account_data *b);
	
	account_data *find;
	int id;
	
	if (acct) {
		id = acct->id;
		HASH_FIND_INT(account_table, &id, find);
		if (!find) {
			HASH_ADD_INT(account_table, id, acct);
			HASH_SORT(account_table, sort_accounts);
		}
	}
}


/**
* Attaches a player to an account. This can't be called until a player has
* an idnum and has been added to the player index.
*
* @param char_data *ch The player to add.
* @param account_data *acct The account to add them to.
*/
void add_player_to_account(char_data *ch, account_data *acct) {
	struct account_player *plr, *pos;
	player_index_data *index;
	bool found;
	
	if (!ch || IS_NPC(ch) || !acct) {
		log("SYSERR: add_player_to_account called without %s", acct ? "player" : "account");
		return;
	}
	
	if (!(index = find_player_index_by_idnum(GET_IDNUM(ch)))) {
		log("SYSERR: add_player_to_account called on player not in index");
		return;
	}
	
	CREATE(plr, struct account_player, 1);
	plr->name = str_dup(GET_PC_NAME(ch));
	strtolower(plr->name);
	plr->player = index;
	
	// see if the player's name is already in the account list (sometimes caused by disconnects during creation)
	found = FALSE;
	for (pos = acct->players; pos; pos = pos->next) {
		if (!str_cmp(pos->name, plr->name)) {
			found = TRUE;
			pos->player = index;
			free(plr->name);
			free(plr);
			plr = pos;
			break;
		}
	}
	
	if (!found) {
		// add to end of account player list
		if ((pos = acct->players)) {
			while (pos->next) {
				pos = pos->next;
			}
			pos->next = plr;
		}
		else {
			acct->players = plr;
		}
	}
	
	save_library_file_for_vnum(DB_BOOT_ACCT, acct->id);
}


/**
* Creates a new account and adds a player to it. The account is added to the
* account_table and saved.
*
* @param char_data *ch The player to make a new account for.
* @return account_data* A pointer to the new account.
*/
account_data *create_account_for_player(char_data *ch) {
	account_data *acct;
	
	if (!ch || IS_NPC(ch)) {
		log("SYSERR: create_account_for_player called without player");
		return NULL;
	}
	if (GET_ACCOUNT(ch)) {
		log("SYSERR: create_account_for_player called for player with account");
		return GET_ACCOUNT(ch);
	}
	
	CREATE(acct, account_data, 1);
	acct->id = ++top_account_id;
	acct->last_logon = ch->player.time.logon;
	
	add_account_to_table(acct);
	add_player_to_account(ch, acct);
	
	save_index(DB_BOOT_ACCT);
	save_library_file_for_vnum(DB_BOOT_ACCT, acct->id);
	
	return acct;
}


/**
* @param int id The account to look up.
* @return account_data* The account from account_table, if any (or NULL).
*/
account_data *find_account(int id) {
	account_data *acct;	
	HASH_FIND_INT(account_table, &id, acct);
	return acct;
}


/**
* Frees the memory for an account.
*
* @param account_data *acct An account to free.
*/
void free_account(account_data *acct) {
	struct account_player *plr;
	
	if (!acct) {
		return;
	}
	
	if (acct->notes) {
		free(acct->notes);
	}
	
	// free players list
	while ((plr = acct->players)) {
		if (plr->name) {
			free(plr->name);
		}
		acct->players = plr->next;
		free(plr);
	}
	
	free(acct);
}


/**
* Reads in one account from a file and adds it to the table.
*
* @param FILE *fl The file to read from.
* @param int nr The id of the account.
*/
void parse_account(FILE *fl, int nr) {
	char err_buf[MAX_STRING_LENGTH], line[256], str_in[256];
	struct account_player *plr, *last_plr = NULL;
	account_data *acct, *find;
	long l_in;
	
	// create
	CREATE(acct, account_data, 1);
	acct->id = nr;
	
	HASH_FIND_INT(account_table, &nr, find);
	if (find) {
		log("WARNING: Duplicate account id #%d", nr);
		// but have to load it anyway to advance the file
	}
	add_account_to_table(acct);
	
	snprintf(err_buf, sizeof(err_buf), "account #%d", nr);
	
	// line 1: last login, flags
	if (get_line(fl, line) && sscanf(line, "%ld %s", &l_in, str_in) == 2) {
		acct->last_logon = l_in;
		acct->flags = asciiflag_conv(str_in);
	}
	else {
		log("SYSERR: Format error in line 1 of %s", err_buf);
		exit(1);
	}
	
	// line 2+: notes
	acct->notes = fread_string(fl, err_buf);
	
	// alphabetic flag section
	snprintf(err_buf, sizeof(err_buf), "account #%d, in alphabetic flags", nr);
	for (;;) {
		if (!get_line(fl, line)) {
			log("SYSERR: Format error in %s", err_buf);
			exit(1);
		}
		switch (*line) {
			case 'P': {	// player
				if (sscanf(line, "P %s", str_in) == 1) {
					CREATE(plr, struct account_player, 1);
					plr->name = str_dup(str_in);
					
					// add to end
					if (last_plr) {
						last_plr->next = plr;
					}
					else {
						acct->players = plr;
					}
					last_plr = plr;
				}
				else {
					log("SYSERR: Format error in P section of %s", err_buf);
					exit(1);
				}
				break;
			}
			case 'S': {	// end
				return;
			}

			default: {
				log("SYSERR: Format error in %s", err_buf);
				exit(1);
			}
		}
	}
}


/**
* Removes an account from the hash table.
*
* @param account_data *acct The account to remove from the table.
*/
void remove_account_from_table(account_data *acct) {
	HASH_DEL(account_table, acct);
}


/**
* Removes a player from their existing account and deletes it if there are no
* more players on the account.
*
* @param char_data *ch The player to remove from its account.
*/
void remove_player_from_account(char_data *ch) {
	struct account_player *plr, *next_plr, *temp;
	player_index_data *index;
	account_data *acct;
	bool has_players;
	
	// sanity checks
	if (!ch || !(acct = GET_ACCOUNT(ch))) {
		return;
	}
	
	// find index entry
	index = find_player_index_by_idnum(GET_IDNUM(ch));
	
	// remove player from list
	has_players = FALSE;
	for (plr = acct->players; plr && index; plr = next_plr) {
		next_plr = plr->next;
		
		if (plr->player == index) {
			// remove
			if (plr->name) {
				free(plr->name);
			}
			REMOVE_FROM_LIST(plr, acct->players, next);
			free(plr);
		}
		else {
			has_players = TRUE;
		}
	}
	
	GET_ACCOUNT(ch) = NULL;
	
	if (!has_players) {
		remove_account_from_table(acct);
		save_index(DB_BOOT_ACCT);
	}

	// save either way
	save_library_file_for_vnum(DB_BOOT_ACCT, acct->id);
	
	if (!has_players) {
		free_account(acct);
	}
}


/**
* @param account_data *a One element
* @param account_data *b Another element
* @return int Sort instruction of -1, 0, or 1
*/
int sort_accounts(account_data *a, account_data *b) {
	return a->id - b->id;
}


/**
* Writes the account index to file.
*
* @param FILE *fl The open index file.
*/
void write_account_index(FILE *fl) {
	account_data *acct, *next_acct;
	int this, last;
	
	last = -1;
	HASH_ITER(hh, account_table, acct, next_acct) {
		// determine "zone number" by vnum
		this = (int)(acct->id / 100);
	
		if (this != last) {
			fprintf(fl, "%d%s\n", this, ACCT_SUFFIX);
			last = this;
		}
	}
}


/**
* Outputs one account in the db file format, starting with a #ID and ending
* in an S.
*
* @param FILE *fl The file to write it to.
* @param account_data *acct The account to save.
*/
void write_account_to_file(FILE *fl, account_data *acct) {
	char temp[MAX_STRING_LENGTH];
	struct account_player *plr;
	
	if (!fl || !acct) {
		syslog(SYS_ERROR, LVL_START_IMM, TRUE, "SYSERR: write_account_to_file called without %s", !fl ? "file" : "account");
		return;
	}
	
	fprintf(fl, "#%d\n", acct->id);
	fprintf(fl, "%ld %s\n", acct->last_logon, bitv_to_alpha(acct->flags));

	strcpy(temp, NULLSAFE(acct->notes));
	strip_crlf(temp);
	fprintf(fl, "%s~\n", temp);
	
	// P: player
	for (plr = acct->players; plr; plr = plr->next) {
		if (plr->player || plr->name) {
			fprintf(fl, "P %s\n", plr->player ? plr->player->name : plr->name);
		}
	}
	
	// END
	fprintf(fl, "S\n");
}


 //////////////////////////////////////////////////////////////////////////////
//// CORE PLAYER DB //////////////////////////////////////////////////////////

/**
* Adds a player to the player tables (by_name and by_idnum), and sorts both
* tables.
*
* @param player_index_data *plr The player to add.
*/
void add_player_to_table(player_index_data *plr) {
	player_index_data *find;
	int idnum = plr->idnum;
	int iter;
	
	// by idnum
	find = NULL;
	HASH_FIND(idnum_hh, player_table_by_idnum, &idnum, sizeof(int), find);
	if (!find) {
		HASH_ADD(idnum_hh, player_table_by_idnum, idnum, sizeof(int), plr);
		HASH_SRT(idnum_hh, player_table_by_idnum, sort_players_by_idnum);
	}
	
	// by name: ensure name is lowercase
	find = NULL;
	for (iter = 0; iter < strlen(plr->name); ++iter) {
		plr->name[iter] = LOWER(plr->name[iter]);
	}
	HASH_FIND(name_hh, player_table_by_name, plr->name, strlen(plr->name), find);
	if (!find) {
		HASH_ADD(name_hh, player_table_by_name, name, strlen(plr->name), plr);
		HASH_SRT(name_hh, player_table_by_name, sort_players_by_name);
	}
}


/**
* Creates the player index by loading all players from the accounts. This must
* be run after accounts are loaded, but before the mud boots up.
*
* This also determines:
*   top_idnum
*   top_account_id
*/
void build_player_index(void) {
	struct account_player *plr, *next_plr, *temp;
	account_data *acct, *next_acct;
	player_index_data *index;
	bool has_players;
	char_data *ch;
	
	HASH_ITER(hh, account_table, acct, next_acct) {
		acct->last_logon = 0;	// reset
		
		// update top account id
		top_account_id = MAX(top_account_id, acct->id);
		
		has_players = FALSE;
		for (plr = acct->players; plr; plr = next_plr) {
			next_plr = plr->next;
			
			if (!plr->player) {
				ch = NULL;
				
				// load the character
				if (plr->name && *plr->name) {
					ch = load_player(plr->name);
				}
				
				// could not load character for this entry
				if (!ch) {
					log("SYSERR: Unable to index account player '%s'", plr->name ? plr->name : "???");
					REMOVE_FROM_LIST(plr, acct->players, next);
					if (plr->name) {
						free(plr->name);
					}
					free(plr);
					continue;
				}
				
				has_players = TRUE;
				
				CREATE(index, player_index_data, 1);
				update_player_index(index, ch);
				add_player_to_table(index);
				plr->player = index;
				
				// detect top idnum
				top_idnum = MAX(top_idnum, GET_IDNUM(ch));
				
				// unload character
				free_char(ch);
			}
			
			// update last logon
			acct->last_logon = MAX(acct->last_logon, plr->player->last_logon);
		}
		
		// failed to load any players -- delete it
		if (!has_players) {
			remove_account_from_table(acct);
			save_index(DB_BOOT_ACCT);
			save_library_file_for_vnum(DB_BOOT_ACCT, acct->id);
			free_account(acct);
		}
	}
}


/* release memory allocated for a char struct */
void free_char(char_data *ch) {
	void free_alias(struct alias_data *a);

	struct channel_history_data *history;
	struct player_slash_channel *slash;
	struct interaction_item *interact;
	struct offer_data *offer;
	struct lore_data *lore;
	struct coin_data *coin;
	struct alias_data *a;
	char_data *proto;
	obj_data *obj;
	int iter;

	// in case somehow?
	if (GROUP(ch)) {
		leave_group(ch);
	}
	
	proto = IS_NPC(ch) ? mob_proto(GET_MOB_VNUM(ch)) : NULL;

	if (IS_NPC(ch)) {
		free_mob_tags(&MOB_TAGGED_BY(ch));
	}

	// This is really just players, but I suppose a mob COULD have it ...
	if (ch->player_specials != NULL && ch->player_specials != &dummy_mob) {		
		if (GET_LASTNAME(ch)) {
			free(GET_LASTNAME(ch));
		}
		if (GET_TITLE(ch)) {
			free(GET_TITLE(ch));
		}
		if (GET_PROMPT(ch)) {
			free(GET_PROMPT(ch));
		}
		if (GET_FIGHT_PROMPT(ch)) {
			free(GET_FIGHT_PROMPT(ch));
		}
		if (POOFIN(ch)) {
			free(POOFIN(ch));
		}
		if (POOFOUT(ch)) {
			free(POOFOUT(ch));
		}
	
		while ((a = GET_ALIASES(ch)) != NULL) {
			GET_ALIASES(ch) = (GET_ALIASES(ch))->next;
			free_alias(a);
		}
		
		while ((offer = GET_OFFERS(ch))) {
			GET_OFFERS(ch) = offer->next;
			free(offer);
		}
		
		while ((slash = GET_SLASH_CHANNELS(ch))) {
			while ((history = slash->history)) {
				slash->history = history->next;
				if (history->message) {
					free(history->message);
				}
				free(history);
			}
			
			GET_SLASH_CHANNELS(ch) = slash->next;
			free(slash);
		}
		
		while ((coin = GET_PLAYER_COINS(ch))) {
			GET_PLAYER_COINS(ch) = coin->next;
			free(coin);
		}
				
		free(ch->player_specials);
		if (IS_NPC(ch)) {
			log("SYSERR: Mob %s (#%d) had player_specials allocated!", GET_NAME(ch), GET_MOB_VNUM(ch));
		}
	}

	if (ch->player.name && (!proto || ch->player.name != proto->player.name)) {
		free(ch->player.name);
	}
	if (ch->player.short_descr && (!proto || ch->player.short_descr != proto->player.short_descr)) {
		free(ch->player.short_descr);
	}
	if (ch->player.long_descr && (!proto || ch->player.long_descr != proto->player.long_descr)) {
		free(ch->player.long_descr);
	}
	if (ch->proto_script && (!proto || ch->proto_script != proto->proto_script)) {
		free_proto_script(ch, MOB_TRIGGER);
	}
	if (ch->interactions && (!proto || ch->interactions != proto->interactions)) {
		while ((interact = ch->interactions)) {
			ch->interactions = interact->next;
			free(interact);
		}
	}
	
	// remove all affects
	while (ch->affected) {
		affect_remove(ch, ch->affected);
	}
	
	// remove cooldowns
	while (ch->cooldowns) {
		remove_cooldown(ch, ch->cooldowns);
	}

	/* free any assigned scripts */
	if (SCRIPT(ch)) {
		extract_script(ch, MOB_TRIGGER);
	}
	
	// TODO what about script memory (freed in extract_char_final for NPCs, but what about PCs?)
	
	// free lore
	while ((lore = GET_LORE(ch))) {
		GET_LORE(ch) = lore->next;
		if (lore->text) {
			free(lore->text);
		}
		free(lore);
	}
	
	// alert empire data the mob is despawned
	if (GET_EMPIRE_NPC_DATA(ch)) {
		GET_EMPIRE_NPC_DATA(ch)->mob = NULL;
		GET_EMPIRE_NPC_DATA(ch) = NULL;
	}
	
	// extract objs
	while ((obj = ch->carrying)) {
		extract_obj(obj);
	}
	for (iter = 0; iter < NUM_WEARS; ++iter) {
		if (GET_EQ(ch, iter)) {
			extract_obj(GET_EQ(ch, iter));
		}
	}

	if (ch->desc)
		ch->desc->character = NULL;

	/* find_char helper */
	remove_from_lookup_table(GET_ID(ch));

	free(ch);
}


/**
* Loads a character from file. This creates a character but does not add them
* to any lists or perform any checks. It also does not load:
* - no aliases
* - no equipment or inventory
* - no script variables
*/
char_data *load_player(char *name) {
	char filename[256];
	char_data *ch;
	FILE *fl;
	
	if (!get_filename(name, filename, PLR_FILE)) {
		log("SYSERR: load_player: Unable to get player filename for '%s'", name);
		return NULL;
	}
	if (!(fl = fopen(filename, "r"))) {
		// no character file exists
		return NULL;
	}
	
	ch = read_player_from_file(fl, name);
	
	fclose(fl);
	return ch;
}


/**
* Parse a player file.
*
* @param FILE *fl The open file, for reading.
* @param char *name The name of the player we are trying to load.
* @return char_data* The loaded character.
*/
char_data *read_player_from_file(FILE *fl, char *name) {
	char line[MAX_INPUT_LENGTH], error[MAX_STRING_LENGTH], str_in[MAX_INPUT_LENGTH];
	struct slash_channel *slash, *last_slash = NULL;
	int account_id = NOTHING, ignore_pos = 0, reward_pos = 0;
	struct over_time_effect_type *dot, *last_dot = NULL;
	struct offer_data *offer, *last_offer = NULL;
	struct lore_data *lore, *last_lore = NULL;
	int length, i_in[7], iter, num;
	struct cooldown_data *cool;
	struct affected_type *af;
	account_data *acct;
	bool end = FALSE;
	char_data *ch;
	double dbl_in;
	long l_in;
	char c_in;
	
	// allocate player
	CREATE(ch, char_data, 1);
	clear_char(ch);
	CREATE(ch->player_specials, struct player_special_data, 1);
	clear_player(ch);
	
	// this is now
	ch->player.time.logon = time(0);
	
	// for fread_string
	sprintf(error, "read_player_from_file: %s", name);
	
	// for more readable if/else chain
	#define PFILE_TAG(src, tag, len)  (!strn_cmp((src), (tag), ((len) = strlen(tag))))
	#define BAD_TAG_WARNING(src)  else if (LOG_BAD_TAG_WARNINGS) { log("SYSERR: Bad tag in player '%s': %s", NULLSAFE(GET_PC_NAME(ch)), (src)); }

	while (!end) {
		if (!get_line(fl, line)) {
			log("SYSERR: Unexpected end of player file in read_player_from_file");
			exit(1);
		}
		
		if (PFILE_TAG(line, "End", length)) {
			end = TRUE;
			continue;
		}
		
		// normal tags by letter
		switch (UPPER(*line)) {
			case 'A': {
				if (PFILE_TAG(line, "Ability:", length)) {
					sscanf(line + length + 1, "%d %d %d", &i_in[0], &i_in[1], &i_in[2]);
					if (i_in[0] >= 0 && i_in[0] < NUM_ABILITIES) {
						ch->player_specials->saved.abilities[i_in[0]].purchased = i_in[1] ? TRUE : FALSE;
						ch->player_specials->saved.abilities[i_in[0]].levels_gained = i_in[2];
					}
				}
				else if (PFILE_TAG(line, "Access Level:", length)) {
					GET_ACCESS_LEVEL(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Account:", length)) {
					account_id = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Action:", length)) {
					if (sscanf(line + length + 1, "%d %d %d %d", &i_in[0], &i_in[1], &i_in[2], &i_in[3]) == 4) {
						GET_ACTION(ch) = i_in[0];
						GET_ACTION_CYCLE(ch) = i_in[1];
						GET_ACTION_TIMER(ch) = i_in[2];
						GET_ACTION_ROOM(ch) = i_in[3];
					}
				}
				else if (PFILE_TAG(line, "Action-vnum:", length)) {
					if (sscanf(line + length + 1, "%d %d", &i_in[0], &i_in[1]) == 2) {
						if (i_in[0] < NUM_ACTION_VNUMS) {
							GET_ACTION_VNUM(ch, i_in[0]) = i_in[1];
						}
					}
				}
				else if (PFILE_TAG(line, "Adventure Summon Loc:", length)) {
					GET_ADVENTURE_SUMMON_RETURN_LOCATION(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Adventure Summon Map:", length)) {
					GET_ADVENTURE_SUMMON_RETURN_MAP(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Affect:", length)) {
					sscanf(line + length + 1, "%d %d %d %d %d %s", &i_in[0], &i_in[1], &i_in[2], &i_in[3], &i_in[4], str_in);
					CREATE(af, struct affected_type, 1);
					af->type = i_in[0];
					af->cast_by = i_in[1];
					af->duration = i_in[2];
					af->modifier = i_in[3];
					af->location = i_in[4];
					af->bitvector = asciiflag_conv(str_in);

					affect_to_char(ch, af);
					free(af);
				}
				else if (PFILE_TAG(line, "Affect Flags:", length)) {
					AFF_FLAGS(ch) = asciiflag_conv(line + length + 1);
				}
				else if (PFILE_TAG(line, "Apparent Age:", length)) {
					GET_APPARENT_AGE(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Archetype:", length)) {
					CREATION_ARCHETYPE(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Attribute-", length)) {
					sscanf(line + length, "%s: %d", str_in, &i_in[0]);
					for (iter = 0; iter < NUM_ATTRIBUTES; ++iter) {
						if (!str_cmp(str_in, attributes[iter].name)) {
							GET_REAL_ATT(ch, iter) = i_in[0];
							GET_ATT(ch, iter) = i_in[0];
							break;
						}
					}
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'B': {
				if (PFILE_TAG(line, "Bad passwords:", length)) {
					GET_BAD_PWS(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Birth:", length)) {
					ch->player.time.birth = atol(line + length + 1);
				}
				else if (PFILE_TAG(line, "Bonus Exp:", length)) {
					GET_DAILY_BONUS_EXPERIENCE(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Bonus Traits:", length)) {
					GET_BONUS_TRAITS(ch) = asciiflag_conv(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'C': {
				if (PFILE_TAG(line, "Can Gain New Skills:", length)) {
					CAN_GAIN_NEW_SKILLS(ch) = atoi(line + length + 1) ? TRUE : FALSE;
				}
				else if (PFILE_TAG(line, "Can Get Bonus Skills:", length)) {
					CAN_GET_BONUS_SKILLS(ch) = atoi(line + length + 1) ? TRUE : FALSE;
				}
				else if (PFILE_TAG(line, "Class:", length)) {
					GET_CLASS(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Class Progression:", length)) {
					GET_CLASS_PROGRESSION(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Class Role:", length)) {
					GET_CLASS_ROLE(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Condition-", length)) {
					sscanf(line + length, "%s: %d", str_in, &i_in[0]);
					if ((num = search_block(str_in, condition_types, TRUE)) != NOTHING) {
						GET_COND(ch, num) = i_in[0];
					}
				}
				else if (PFILE_TAG(line, "Confused Direction:", length)) {
					GET_CONFUSED_DIR(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Cooldown:", length)) {
					sscanf(line + length + 1, "%d %ld", &i_in[0], &l_in);
					CREATE(cool, struct cooldown_data, 1);
					add_cooldown(ch, i_in[0], l_in - time(0));
				}
				else if (PFILE_TAG(line, "Creation Host:", length)) {
					if (GET_CREATION_HOST(ch)) {
						free(GET_CREATION_HOST(ch));
					}
					GET_CREATION_HOST(ch) = str_dup(trim(line + length + 1));
				}
				else if (PFILE_TAG(line, "Current-", length)) {
					sscanf(line + length, "%s: %d", str_in, &i_in[0]);
					if ((num = search_block(str_in, pool_types, TRUE)) != NOTHING) {
						GET_CURRENT_POOL(ch, num) = i_in[0];
					}
				}
				else if (PFILE_TAG(line, "Color-", length)) {
					sscanf(line + length, "%s: %c", str_in, &c_in);
					if ((num = search_block(str_in, custom_color_types, TRUE)) != NOTHING) {
						GET_CUSTOM_COLOR(ch, num) = c_in;
					}
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'D': {
				if (PFILE_TAG(line, "Daily Cycle:", length)) {
					GET_DAILY_CYCLE(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Deficit-", length)) {
					sscanf(line + length, "%s: %d", str_in, &i_in[0]);
					if ((num = search_block(str_in, pool_types, TRUE)) != NOTHING) {
						GET_DEFICIT(ch, num) = i_in[0];
					}
				}
				else if (PFILE_TAG(line, "Description:", length)) {
					if (GET_LONG_DESC(ch)) {
						free(GET_LONG_DESC(ch));
					}
					GET_LONG_DESC(ch) = fread_string(fl, error);
				}
				else if (PFILE_TAG(line, "Disguised Name:", length)) {
					if (GET_DISGUISED_NAME(ch)) {
						free(GET_DISGUISED_NAME(ch));
					}
					GET_DISGUISED_NAME(ch) = str_dup(trim(line + length + 1));
				}
				else if (PFILE_TAG(line, "Disguised Sex:", length)) {
					if ((num = search_block(trim(line + length + 1), genders, TRUE)) != NOTHING) {
						GET_DISGUISED_SEX(ch) = num;
					}
				}
				else if (PFILE_TAG(line, "DoT Effect:", length)) {
					sscanf(line + length + 1, "%d %d %d %d %d %d %d", &i_in[0], &i_in[1], &i_in[2], &i_in[3], &i_in[4], &i_in[5], &i_in[6]);
					CREATE(dot, struct over_time_effect_type, 1);
					dot->type = i_in[0];
					dot->cast_by = i_in[1];
					dot->duration = i_in[2];
					dot->damage_type = i_in[3];
					dot->damage = i_in[4];
					dot->stack = i_in[5];
					dot->max_stack = i_in[6];
					
					// append to end
					if (last_dot) {
						last_dot->next = dot;
					}
					else {
						ch->over_time_effects = dot;
					}
					last_dot = dot;
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'E': {
				if (PFILE_TAG(line, "Empire:", length)) {
					GET_LOYALTY(ch) = real_empire(atoi(line + length + 1));
				}
				else if (PFILE_TAG(line, "Extra-", length)) {
					sscanf(line + length, "%s: %d", str_in, &i_in[0]);
					if ((num = search_block(str_in, extra_attribute_types, TRUE)) != NOTHING) {
						GET_EXTRA_ATT(ch, num) = i_in[0];
					}
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'F': {
				if (PFILE_TAG(line, "Fight Prompt:", length)) {
					if (GET_FIGHT_PROMPT(ch)) {
						free(GET_FIGHT_PROMPT(ch));
					}
					GET_FIGHT_PROMPT(ch) = str_dup(line + length + 1);	// do not trim
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'G': {
				if (PFILE_TAG(line, "Grants:", length)) {
					GET_GRANT_FLAGS(ch) = asciiflag_conv(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'H': {
				if (PFILE_TAG(line, "Highest Known Level:", length)) {
					GET_HIGHEST_KNOWN_LEVEL(ch) = atoi(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'I': {
				if (PFILE_TAG(line, "Idnum:", length)) {
					GET_IDNUM(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Ignore:", length)) {
					if (ignore_pos < MAX_IGNORES) {
						GET_IGNORE_LIST(ch, ignore_pos++) = atoi(line + length + 1);
					}
				}
				else if (PFILE_TAG(line, "Immortal Level:", length)) {
					GET_IMMORTAL_LEVEL(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Injuries:", length)) {
					INJURY_FLAGS(ch) = asciiflag_conv(line + length + 1);
				}
				else if (PFILE_TAG(line, "Invis Level:", length)) {
					GET_INVIS_LEV(ch) = atoi(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'L': {
				if (PFILE_TAG(line, "Lastname:", length)) {
					if (GET_LASTNAME(ch)) {
						free(GET_LASTNAME(ch));
					}
					GET_LASTNAME(ch) = str_dup(trim(line + length + 1));
				}
				else if (PFILE_TAG(line, "Last Host:", length)) {
					if (ch->prev_host) {
						free(ch->prev_host);
					}
					ch->prev_host = str_dup(trim(line + length + 1));
				}
				else if (PFILE_TAG(line, "Last Known Level:", length)) {
					GET_LAST_KNOWN_LEVEL(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Last Logon:", length)) {
					ch->prev_logon = atol(line + length + 1);
				}
				else if (PFILE_TAG(line, "Last Tell:", length)) {
					GET_LAST_TELL(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Last Tip:", length)) {
					GET_LAST_TIP(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Last Room:", length)) {
					GET_LAST_ROOM(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Last Direction:", length)) {
					GET_LAST_DIR(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Last Death:", length)) {
					GET_LAST_DEATH_TIME(ch) = atol(line + length + 1);
				}
				else if (PFILE_TAG(line, "Last Corpse Id:", length)) {
					GET_LAST_CORPSE_ID(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Load Room:", length)) {
					GET_LOADROOM(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Load Room Check:", length)) {
					GET_LOAD_ROOM_CHECK(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Lore:", length)) {
					sscanf(line + length + 1, "%d %ld", &i_in[0], &l_in);
					CREATE(lore, struct lore_data, 1);
					lore->type = i_in[0];
					lore->date = l_in;
					
					// text on next line
					if (get_line(fl, line)) {
						lore->text = str_dup(line);
					}
					
					// append to end
					if (last_lore) {
						last_lore->next = lore;
					}
					else {
						GET_LORE(ch) = lore;
					}
					last_lore = lore;
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'M': {
				if (PFILE_TAG(line, "Map Mark:", length)) {
					GET_MARK_LOCATION(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Mapsize:", length)) {
					GET_MAPSIZE(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Max-", length)) {
					sscanf(line + length, "%s: %d", str_in, &i_in[0]);
					if ((num = search_block(str_in, pool_types, TRUE)) != NOTHING) {
						GET_MAX_POOL(ch, num) = i_in[0];
					}
				}
				else if (PFILE_TAG(line, "Morph:", length)) {
					GET_MORPH(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Mount Flags:", length)) {
					GET_MOUNT_FLAGS(ch) = asciiflag_conv(line + length + 1);
				}
				else if (PFILE_TAG(line, "Mount Vnum:", length)) {
					GET_MOUNT_VNUM(ch) = atoi(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'N': {
				if (PFILE_TAG(line, "Name:", length)) {
					if (GET_PC_NAME(ch)) {
						free(GET_PC_NAME(ch));
					}
					GET_PC_NAME(ch) = str_dup(trim(line + length + 1));
				}
				else if (PFILE_TAG(line, "Notes:", length)) {
					if (GET_ADMIN_NOTES(ch)) {
						free(GET_ADMIN_NOTES(ch));
					}
					GET_ADMIN_NOTES(ch) = fread_string(fl, error);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'O': {
				if (PFILE_TAG(line, "Offer:", length)) {
					sscanf(line + length + 1, "%d %d %d %ld %d", &i_in[0], &i_in[1], &i_in[2], &l_in, &i_in[3]);
					CREATE(offer, struct offer_data, 1);
					offer->from = i_in[0];
					offer->type = i_in[1];
					offer->location = i_in[2];
					offer->time = l_in;
					offer->data = i_in[3];
					
					// append to end
					if (last_offer) {
						last_offer->next = offer;
					}
					else {
						GET_OFFERS(ch) = offer;
					}
					last_offer = offer;
				}
				else if (PFILE_TAG(line, "OLC:", length)) {
					sscanf(line + length + 1, "%d %d %s", &i_in[0], &i_in[1], str_in);
					GET_OLC_MIN_VNUM(ch) = i_in[0];
					GET_OLC_MAX_VNUM(ch) = i_in[1];
					GET_OLC_FLAGS(ch) = asciiflag_conv(str_in);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'P': {
				if (PFILE_TAG(line, "Password:", length)) {
					if (GET_PASSWD(ch)) {
						free(GET_PASSWD(ch));
					}
					GET_PASSWD(ch) = str_dup(line + length + 1);
				}
				else if (PFILE_TAG(line, "Played:", length)) {
					ch->player.time.played = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Player Flags:", length)) {
					PLR_FLAGS(ch) = asciiflag_conv(line + length + 1);
				}
				else if (PFILE_TAG(line, "Pledge Empire:", length)) {
					GET_PLEDGE(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Poofin:", length)) {
					if (POOFIN(ch)) {
						free(POOFIN(ch));
					}
					POOFIN(ch) = str_dup(line + length + 1);	// do not trim
				}
				else if (PFILE_TAG(line, "Poofout:", length)) {
					if (POOFOUT(ch)) {
						free(POOFOUT(ch));
					}
					POOFOUT(ch) = str_dup(line + length + 1);	// do not trim
				}
				else if (PFILE_TAG(line, "Preferences:", length)) {
					PRF_FLAGS(ch) = asciiflag_conv(line + length + 1);
				}
				else if (PFILE_TAG(line, "Promo ID:", length)) {
					GET_PROMO_ID(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Prompt:", length)) {
					if (GET_PROMPT(ch)) {
						free(GET_PROMPT(ch));
					}
					GET_PROMPT(ch) = str_dup(line + length + 1);	// do not trim
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'R': {
				if (PFILE_TAG(line, "Rank:", length)) {
					GET_RANK(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Recent Deaths:", length)) {
					GET_RECENT_DEATH_COUNT(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Referred by:", length)) {
					if (GET_REFERRED_BY(ch)) {
						free(GET_REFERRED_BY(ch));
					}
					GET_REFERRED_BY(ch) = str_dup(trim(line + length + 1));
				}
				else if (PFILE_TAG(line, "Resource:", length)) {
					sscanf(line + length + 1, "%d %s", &i_in[0], str_in);
					for (iter = 0; iter < NUM_MATERIALS; ++iter) {
						if (!str_cmp(str_in, materials[iter].name)) {
							GET_RESOURCE(ch, iter) = i_in[0];
							break;
						}
					}
				}
				else if (PFILE_TAG(line, "Rewarded:", length)) {
					if (reward_pos < MAX_REWARDS_PER_DAY) {
						GET_REWARDED_TODAY(ch, reward_pos++) = atoi(line + length + 1);
					}
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'S': {
				if (PFILE_TAG(line, "Sex:", length)) {
					if ((num = search_block(trim(line + length + 1), genders, TRUE)) != NOTHING) {
						GET_REAL_SEX(ch) = num;
					}
				}
				else if (PFILE_TAG(line, "Skill:", length)) {
					sscanf(line + length + 1, "%d %d %lf %d %d", &i_in[0], &i_in[1], &dbl_in, &i_in[2], &i_in[3]);
					if (i_in[0] >= 0 && i_in[0] < NUM_SKILLS) {
						ch->player_specials->saved.skills[i_in[0]].level = i_in[1];
						GET_SKILL_EXP(ch, i_in[0]) = dbl_in;
						GET_FREE_SKILL_RESETS(ch, i_in[0]) = i_in[2];
						NOSKILL_BLOCKED(ch, i_in[0]) = i_in[3] ? TRUE : FALSE;
					}
				}
				else if (PFILE_TAG(line, "Skill Level:", length)) {
					GET_SKILL_LEVEL(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Slash-channel:", length)) {
					CREATE(slash, struct slash_channel, 1);
					slash->name = str_dup(trim(line + length + 1));
					
					// append to end
					if (last_slash) {
						last_slash->next = slash;
					}
					else {
						LOAD_SLASH_CHANNELS(ch) = slash;
					}
					last_slash = slash;
				}
				else if (PFILE_TAG(line, "Syslog Flags:", length)) {
					SYSLOG_FLAGS(ch) = asciiflag_conv(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'T': {
				if (PFILE_TAG(line, "Temporary Account:", length)) {
					GET_TEMPORARY_ACCOUNT_ID(ch) = atoi(line + length + 1);
				}
				else if (PFILE_TAG(line, "Title:", length)) {
					if (GET_TITLE(ch)) {
						free(GET_TITLE(ch));
					}
					GET_TITLE(ch) = str_dup(line + length + 1);	// do not trim
				}
				else if (PFILE_TAG(line, "Tomb Room:", length)) {
					GET_TOMB_ROOM(ch) = atoi(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			case 'U': {
				if (PFILE_TAG(line, "Using Poison:", length)) {
					USING_POISON(ch) = atoi(line + length + 1);
				}
				BAD_TAG_WARNING(line);
				break;
			}
			
			default: {
				// ignore anything else and move on
				if (FALSE) { /* for BAD_TAG_WARNING */ }
				BAD_TAG_WARNING(line);
				break;
			}
		}
	}
	
	if (!GET_PC_NAME(ch) || !*GET_PC_NAME(ch)) {
		log("SYSERR: Finished loading playerfile '%s' but did not find name", name);
		// hopefully we came in with a good name
		GET_PC_NAME(ch) = str_dup((name && *name) ? name : "Unknown");
		CAP(GET_PC_NAME(ch));
	}
	if (GET_IDNUM(ch) <= 0) {
		log("SYSERR: Finished loading playerfile '%s' but did not find idnum", GET_PC_NAME(ch));
		// TODO assign new idnum?
	}
	if (!GET_PASSWD(ch) || !*GET_PASSWD(ch)) {
		log("SYSERR: Finished loading playerfile '%s' but did not find password", GET_PC_NAME(ch));
	}
	
	// have account?
	if (!(acct = find_account(account_id))) {
		acct = create_account_for_player(ch);
	}
	GET_ACCOUNT(ch) = acct;
	
	// safety
	REMOVE_BIT(PLR_FLAGS(ch), PLR_EXTRACTED | PLR_DONTSET);
	
	// Players who have been out for 1 hour get a free restore
	RESTORE_ON_LOGIN(ch) = (((int) (time(0) - ch->prev_logon)) >= 1 * SECS_PER_REAL_HOUR);
	REREAD_EMPIRE_TECH_ON_LOGIN(ch) = member_is_timed_out(ch->player.time.birth, ch->prev_logon, ((double)ch->player.time.played) / SECS_PER_REAL_HOUR);
	
	return ch;
}


/**
* Removes a player from the player tables.
*
* @param player_index_data *plr The player to remove.
*/
void remove_player_from_table(player_index_data *plr) {
	HASH_DELETE(idnum_hh, player_table_by_idnum, plr);
	HASH_DELETE(name_hh, player_table_by_name, plr);
}


/*
 * write the vital data of a player to the player file -- this will not save
 * players who are disconnected.
 *
 * @param char_data *ch The player to save.
 * @param room_data *load_room (Optional) The location that the player will reappear on reconnect.
 */
void save_char(char_data *ch, room_data *load_room) {
	void save_char_vars(char_data *ch);
	
	char filename[256], tempname[256];
	player_index_data *index;
	room_data *map;
	FILE *fl;

	if (IS_NPC(ch)) {
		return;
	}
	
	// update load room if they aren't flagged for a static one
	if (!PLR_FLAGGED(ch, PLR_LOADROOM)) {
		if (load_room) {
			GET_LOADROOM(ch) = GET_ROOM_VNUM(load_room);
			map = get_map_location_for(load_room);
			GET_LOAD_ROOM_CHECK(ch) = (map ? GET_ROOM_VNUM(map) : NOWHERE);
		}
		else {
			GET_LOADROOM(ch) = NOWHERE;
		}
	}
	
	if (!get_filename(GET_PC_NAME(ch), filename, PLR_FILE)) {
		log("SYSERR: save_char: Unable to get player filename for '%s'", GET_PC_NAME(ch));
		return;
	}
	
	// store to a temp name in order to avoid problems from crashes during save
	snprintf(tempname, sizeof(tempname), "%s%s", filename, TEMP_SUFFIX);
	if (!(fl = fopen(tempname, "w"))) {
		log("SYSERR: save_char: Unable to open '%s' for writing", tempname);
		return;
	}
	
	// write it
	write_player_to_file(fl, ch);
	
	fclose(fl);
	rename(tempname, filename);
	
	// additional data to save
	save_char_vars(ch);
	
	// update the index in case any of this changed
	index = find_player_index_by_idnum(GET_IDNUM(ch));
	update_player_index(index, ch);
}


/**
* @param player_index_data *a One element
* @param player_index_data *b Another element
* @return int Sort instruction of -1, 0, or 1
*/
int sort_players_by_idnum(player_index_data *a, player_index_data *b) {
	return a->idnum - b->idnum;
}


/**
* @param player_index_data *a One element
* @param player_index_data *b Another element
* @return int Sort instruction of -1, 0, or 1
*/
int sort_players_by_name(player_index_data *a, player_index_data *b) {
	return strcmp(a->name, b->name);
}


/**
* For commands which load chars from file: this handles writing the output
* and frees the character. This should be used on a character loaded via
* find_or_load_player().
*
* ONLY use this if the character was loaded from file for a command like "set"
*
* @param char_data *ch the loaded char -- will be freed
*/
void store_loaded_char(char_data *ch) {
	save_char(ch, real_room(GET_LOADROOM(ch)));
	free_char(ch);
}


/**
* Updates the player index entry for the character. You must look the index
* up first, as this can be used before it's added to the player table.
*
* @param plar_index_data *index The index entry to update.
* @param char_data *ch The player character to update it with.
*/
void update_player_index(player_index_data *index, char_data *ch) {
	if (!index || !ch) {
		return;
	}
	
	index->idnum = GET_IDNUM(ch);
	
	if (index->name) {
		free(index->name);
	}
	index->name = str_dup(GET_PC_NAME(ch));
	strtolower(index->name);
	
	if (index->fullname) {
		free(index->fullname);
	}
	index->fullname = PERS(ch, ch, TRUE);
	
	index->account_id = GET_ACCOUNT(ch)->id;
	index->last_logon = ch->prev_logon;
	index->birth = ch->player.time.birth;
	index->played = ch->player.time.played;
	index->access_level = GET_ACCESS_LEVEL(ch);
	index->plr_flags = PLR_FLAGS(ch);
	index->loyalty = GET_LOYALTY(ch);
	
	if (ch->desc || ch->prev_host) {
		if (index->last_host) {
			free(index->last_host);
		}
		index->last_host = str_dup(ch->desc ? ch->desc->host : ch->prev_host);
	}
}


/**
* Writes all the tagged data for one player to file.
*
* @param FILE *fl The open file to write to.
* @param char_data *ch The player to write.
*/
void write_player_to_file(FILE *fl, char_data *ch) {
	extern struct slash_channel *find_slash_channel_by_id(int id);
	
	struct affected_type *af, *new_af, *next_af, *af_list;
	struct player_slash_channel *slash;
	struct over_time_effect_type *dot;
	struct slash_channel *loadslash;
	struct slash_channel *channel;
	obj_data *char_eq[NUM_WEARS];
	char temp[MAX_STRING_LENGTH];
	struct cooldown_data *cool;
	struct offer_data *offer;
	struct lore_data *lore;
	int iter;
	
	if (!fl || !ch) {
		log("SYSERR: write_player_to_file called without %s", fl ? "character" : "file");
		return;
	}
	if (IS_NPC(ch)) {
		log("SYSERR: write_player_to_file called with NPC");
		return;
	}
	
	// unaffect the character to store raw numbers: equipment
	for (iter = 0; iter < NUM_WEARS; ++iter) {
		if (GET_EQ(ch, iter)) {
			char_eq[iter] = unequip_char(ch, iter);
			#ifndef NO_EXTRANEOUS_TRIGGERS
				remove_otrigger(char_eq[iter], ch);
			#endif
		}
		else {
			char_eq[iter] = NULL;
		}
	}
	
	// unaffect: affects
	af_list = NULL;
	while ((af = ch->affected)) {
		if (af->type > ATYPE_RESERVED && af->type < NUM_ATYPES) {
			CREATE(new_af, struct affected_type, 1);
			*new_af = *af;
			new_af->next = af_list;
			af_list = new_af;
		}
		affect_remove(ch, af);
	}
	
	// reset attributes
	for (iter = 0; iter < NUM_ATTRIBUTES; ++iter) {
		ch->aff_attributes[iter] = ch->real_attributes[iter];
	}
	
	// BEGIN TAGS
	
	// Player info
	fprintf(fl, "Name: %s\n", GET_PC_NAME(ch));
	fprintf(fl, "Password: %s\n", GET_PASSWD(ch));
	fprintf(fl, "Idnum: %d\n", GET_IDNUM(ch));
	if (GET_ACCOUNT(ch)) {
		fprintf(fl, "Account: %d\n", GET_ACCOUNT(ch)->id);
	}
	if (GET_TEMPORARY_ACCOUNT_ID(ch) != NOTHING) {
		fprintf(fl, "Temporary Account: %d\n", GET_TEMPORARY_ACCOUNT_ID(ch));
	}
	
	// Empire info
	if (GET_LOYALTY(ch)) {
		fprintf(fl, "Empire: %d\n", EMPIRE_VNUM(GET_LOYALTY(ch)));
		fprintf(fl, "Rank: %d\n", GET_RANK(ch));
	}
	else {
		if (GET_PLEDGE(ch) != NOTHING) {
			fprintf(fl, "Pledge Empire: %d\n", GET_PLEDGE(ch));
		}
	}
	
	// Last login info
	if (PLR_FLAGGED(ch, PLR_KEEP_LAST_LOGIN_INFO)) {
		// player was loaded from file
		fprintf(fl, "Last Host: %s\n", NULLSAFE(ch->prev_host));
		fprintf(fl, "Last Logon: %ld\n", ch->prev_logon);
	}
	else {
		ch->player.time.played += (int)(time(0) - ch->player.time.logon);
		ch->player.time.logon = time(0);	// reset this first
		fprintf(fl, "Last Host: %s\n", ch->desc ? ch->desc->host : NULLSAFE(ch->prev_host));
		fprintf(fl, "Last Logon: %ld\n", ch->player.time.logon);
	}
	
	// Pools
	for (iter = 0; iter < NUM_POOLS; ++iter) {
		fprintf(fl, "Current-%s: %d\n", pool_types[iter], GET_CURRENT_POOL(ch, iter));
		fprintf(fl, "Max-%s: %d\n", pool_types[iter], GET_MAX_POOL(ch, iter));
		if (GET_DEFICIT(ch, iter)) {
			fprintf(fl, "Deficit-%s: %d\n", pool_types[iter], GET_DEFICIT(ch, iter));
		}
	}
	
	// The rest is alphabetical:
	
	// 'A'
	for (iter = 0; iter < NUM_ABILITIES; ++iter) {
		if (HAS_ABILITY(ch, iter) || GET_LEVELS_GAINED_FROM_ABILITY(ch, iter) > 0) {
			fprintf(fl, "Ability: %d %d %d\n", iter, HAS_ABILITY(ch, iter) ? 1 : 0, GET_LEVELS_GAINED_FROM_ABILITY(ch, iter));
		}
	}
	fprintf(fl, "Access Level: %d\n", GET_ACCESS_LEVEL(ch));
	if (GET_ACTION(ch) != ACT_NONE) {
		fprintf(fl, "Action: %d %d %d %d\n", GET_ACTION(ch), GET_ACTION_CYCLE(ch), GET_ACTION_TIMER(ch), GET_ACTION_ROOM(ch));
		for (iter = 0; iter < NUM_ACTION_VNUMS; ++iter) {
			fprintf(fl, "Action-vnum: %d %d\n", iter, GET_ACTION_VNUM(ch, iter));
		}
	}
	if (GET_ADVENTURE_SUMMON_RETURN_LOCATION(ch) != NOWHERE) {
		fprintf(fl, "Adventure Summon Loc: %d\n", GET_ADVENTURE_SUMMON_RETURN_LOCATION(ch));
		fprintf(fl, "Adventure Summon Map: %d\n", GET_ADVENTURE_SUMMON_RETURN_MAP(ch));
	}
	for (af = af_list; af; af = af->next) {	// stored earlier
		fprintf(fl, "Affect: %d %d %d %d %d %s\n", af->type, af->cast_by, af->duration, af->modifier, af->location, bitv_to_alpha(af->bitvector));
	}
	fprintf(fl, "Affect Flags: %s\n", bitv_to_alpha(AFF_FLAGS(ch)));
	if (GET_APPARENT_AGE(ch)) {
		fprintf(fl, "Apparent Age: %d\n", GET_APPARENT_AGE(ch));
	}
	fprintf(fl, "Archetype: %d\n", CREATION_ARCHETYPE(ch));
	for (iter = 0; iter < NUM_ATTRIBUTES; ++iter) {
		fprintf(fl, "Attribute-%s: %d\n", attributes[iter].name, GET_REAL_ATT(ch, iter));
	}
	
	// 'B'
	if (GET_BAD_PWS(ch)) {
		fprintf(fl, "Bad passwords: %d\n", GET_BAD_PWS(ch));
	}
	fprintf(fl, "Birth: %ld\n", ch->player.time.birth);
	fprintf(fl, "Bonus Exp: %d\n", GET_DAILY_BONUS_EXPERIENCE(ch));
	fprintf(fl, "Bonus Traits: %s\n", bitv_to_alpha(GET_BONUS_TRAITS(ch)));
	
	// 'C'
	if (CAN_GAIN_NEW_SKILLS(ch)) {
		fprintf(fl, "Can Gain New Skills: 1\n");
	}
	if (CAN_GET_BONUS_SKILLS(ch)) {
		fprintf(fl, "Can Get Bonus Skills: 1\n");
	}
	fprintf(fl, "Class: %d\n", GET_CLASS(ch));
	fprintf(fl, "Class Progression: %d\n", GET_CLASS_PROGRESSION(ch));
	fprintf(fl, "Class Role: %d\n", GET_CLASS_ROLE(ch));
	for (iter = 0; iter < NUM_CUSTOM_COLORS; ++iter) {
		if (GET_CUSTOM_COLOR(ch, iter) != 0) {
			fprintf(fl, "Color-%s: %c\n", custom_color_types[iter], GET_CUSTOM_COLOR(ch, iter));
		}
	}
	for (iter = 0; iter < NUM_CONDS; ++iter) {
		if (GET_COND(ch, iter) != 0) {
			fprintf(fl, "Condition-%s: %d\n", condition_types[iter], GET_COND(ch, iter));
		}
	}
	if (GET_CONFUSED_DIR(ch)) {
		fprintf(fl, "Confused Direction: %d\n", GET_CONFUSED_DIR(ch));
	}
	for (cool = ch->cooldowns; cool; cool = cool->next) {
		fprintf(fl, "Cooldown: %d %ld\n", cool->type, cool->expire_time);
	}
	if (GET_CREATION_HOST(ch)) {
		fprintf(fl, "Creation Host: %s\n", GET_CREATION_HOST(ch));
	}
	
	// 'D'
	fprintf(fl, "Daily Cycle: %d\n", GET_DAILY_CYCLE(ch));
	if (GET_LONG_DESC(ch)) {
		strcpy(temp, NULLSAFE(GET_LONG_DESC(ch)));
		strip_crlf(temp);
		fprintf(fl, "Description:\n%s~\n", temp);
	}
	if (GET_DISGUISED_NAME(ch)) {
		fprintf(fl, "Disguised Name: %s\n", GET_DISGUISED_NAME(ch));
	}
	if (GET_DISGUISED_SEX(ch)) {
		fprintf(fl, "Disguised Sex: %s\n", genders[(int) GET_DISGUISED_SEX(ch)]);
	}
	for (dot = ch->over_time_effects; dot; dot = dot->next) {
		fprintf(fl, "DoT Effect: %d %d %d %d %d %d %d\n", dot->type, dot->cast_by, dot->duration, dot->damage_type, dot->damage, dot->stack, dot->max_stack);
	}
	
	// 'E'
	for (iter = 0; iter < NUM_EXTRA_ATTRIBUTES; ++iter) {
		if (GET_EXTRA_ATT(ch, iter)) {
			fprintf(fl, "Extra-%s: %d\n", extra_attribute_types[iter], GET_EXTRA_ATT(ch, iter));
		}
	}
	
	// 'F'
	if (GET_FIGHT_PROMPT(ch)) {
		fprintf(fl, "Fight Prompt: %s\n", GET_FIGHT_PROMPT(ch));
	}
	
	// 'G'
	if (GET_GRANT_FLAGS(ch)) {
		fprintf(fl, "Grants: %s\n", bitv_to_alpha(GET_GRANT_FLAGS(ch)));
	}
	
	// 'H'
	fprintf(fl, "Highest Known Level: %d\n", GET_HIGHEST_KNOWN_LEVEL(ch));
	
	// 'I'
	for (iter = 0; iter < MAX_IGNORES; ++iter) {
		if (GET_IGNORE_LIST(ch, iter) > 0) {
			fprintf(fl, "Ignore: %d\n", GET_IGNORE_LIST(ch, iter));
		}
	}
	if (GET_IMMORTAL_LEVEL(ch)) {
		fprintf(fl, "Immortal Level: %d\n", GET_IMMORTAL_LEVEL(ch));
	}
	fprintf(fl, "Injuries: %s\n", bitv_to_alpha(INJURY_FLAGS(ch)));
	if (GET_INVIS_LEV(ch)) {
		fprintf(fl, "Invis Level: %d\n", GET_INVIS_LEV(ch));
	}
	
	// 'L'
	if (GET_LAST_CORPSE_ID(ch) > 0) {
		fprintf(fl, "Last Corpse Id: %d\n", GET_LAST_CORPSE_ID(ch));
	}
	fprintf(fl, "Last Death: %ld\n", GET_LAST_DEATH_TIME(ch));
	fprintf(fl, "Last Direction: %d\n", GET_LAST_DIR(ch));
	fprintf(fl, "Last Known Level: %d\n", GET_LAST_KNOWN_LEVEL(ch));
	fprintf(fl, "Last Room: %d\n", GET_LAST_ROOM(ch));
	if (GET_LAST_TELL(ch) != NOBODY) {
		fprintf(fl, "Last Tell: %d\n", GET_LAST_TELL(ch));
	}
	if (GET_LAST_TIP(ch)) {
		fprintf(fl, "Last Tip: %d\n", GET_LAST_TIP(ch));
	}
	if (GET_LASTNAME(ch)) {
		fprintf(fl, "Lastname: %s\n", GET_LASTNAME(ch));
	}
	fprintf(fl, "Load Room: %d\n", GET_LOADROOM(ch));
	fprintf(fl, "Load Room Check: %d\n", GET_LOAD_ROOM_CHECK(ch));
	for (lore = GET_LORE(ch); lore; lore = lore->next) {
		if (lore->text && *lore->text) {
			fprintf(fl, "Lore: %d %ld\n%s\n", lore->type, lore->date, lore->text);
		}
	}
	
	// 'M'
	if (GET_MARK_LOCATION(ch) != NOWHERE) {
		fprintf(fl, "Map Mark: %d\n", GET_MARK_LOCATION(ch));
	}
	if (GET_MAPSIZE(ch)) {
		fprintf(fl, "Mapsize: %d\n", GET_MAPSIZE(ch));
	}
	if (GET_MORPH(ch) != MORPH_NONE) {
		fprintf(fl, "Morph: %d\n", GET_MORPH(ch));
	}
	if (GET_MOUNT_FLAGS(ch) != NOBITS) {
		fprintf(fl, "Mount Flags: %s\n", bitv_to_alpha(GET_MOUNT_FLAGS(ch)));
	}
	if (GET_MOUNT_VNUM(ch) != NOTHING) {
		fprintf(fl, "Mount Vnum: %d\n", GET_MOUNT_VNUM(ch));
	}
	
	// 'N'
	if (GET_ADMIN_NOTES(ch)) {
		strcpy(temp, NULLSAFE(GET_ADMIN_NOTES(ch)));
		strip_crlf(temp);
		fprintf(fl, "Notes:\n%s~\n", temp);
	}
	
	// 'O'
	for (offer = GET_OFFERS(ch); offer; offer = offer->next) {
		fprintf(fl, "Offer: %d %d %d %ld %d\n", offer->from, offer->type, offer->location, offer->time, offer->data);
	}
	if (GET_OLC_MAX_VNUM(ch) > 0 || GET_OLC_MIN_VNUM(ch) > 0 || GET_OLC_FLAGS(ch) != NOBITS) {
		fprintf(fl, "OLC: %d %d %s\n", GET_OLC_MIN_VNUM(ch), GET_OLC_MAX_VNUM(ch), bitv_to_alpha(GET_OLC_FLAGS(ch)));
	}
	
	// 'P'
	fprintf(fl, "Played: %d\n", ch->player.time.played);
	fprintf(fl, "Player Flags: %s\n", bitv_to_alpha(PLR_FLAGS(ch)));
	if (POOFIN(ch)) {
		fprintf(fl, "Poofin: %s\n", POOFIN(ch));
	}
	if (POOFOUT(ch)) {
		fprintf(fl, "Poofout: %s\n", POOFOUT(ch));
	}
	if (PRF_FLAGS(ch)) {
		fprintf(fl, "Preferences: %s\n", bitv_to_alpha(PRF_FLAGS(ch)));
	}
	if (GET_PROMO_ID(ch)) {
		fprintf(fl, "Promo ID: %d\n", GET_PROMO_ID(ch));
	}
	if (GET_PROMPT(ch)) {
		fprintf(fl, "Prompt: %s\n", GET_PROMPT(ch));
	}
	
	// 'R'
	if (GET_RECENT_DEATH_COUNT(ch)) {
		fprintf(fl, "Recent Deaths: %d\n", GET_RECENT_DEATH_COUNT(ch));
	}
	if (GET_REFERRED_BY(ch)) {
		fprintf(fl, "Referred by: %s\n", GET_REFERRED_BY(ch));
	}
	for (iter = 0; iter < NUM_MATERIALS; ++iter) {
		if (GET_RESOURCE(ch, iter) != 0) {
			fprintf(fl, "Resource: %d %s\n", GET_RESOURCE(ch, iter), materials[iter].name);
		}
	}
	for (iter = 0; iter < MAX_REWARDS_PER_DAY; ++iter) {
		if (GET_REWARDED_TODAY(ch, iter)) {
			fprintf(fl, "Rewarded: %d\n", GET_REWARDED_TODAY(ch, iter));
		}
	}
	
	// 'S'
	fprintf(fl, "Sex: %s\n", genders[(int) GET_REAL_SEX(ch)]);
	for (iter = 0; iter < NUM_SKILLS; ++iter) {
		fprintf(fl, "Skill: %d %d %.2f %d %d\n", iter, GET_SKILL(ch, iter), GET_SKILL_EXP(ch, iter), GET_FREE_SKILL_RESETS(ch, iter), NOSKILL_BLOCKED(ch, iter) ? 1 : 0);
	}
	fprintf(fl, "Skill Level: %d\n", GET_SKILL_LEVEL(ch));
	for (slash = GET_SLASH_CHANNELS(ch); slash; slash = slash->next) {
		if ((channel = find_slash_channel_by_id(slash->id))) {
			fprintf(fl, "Slash-channel: %s\n", channel->name);
		}
	}
	for (loadslash = LOAD_SLASH_CHANNELS(ch); loadslash; loadslash = loadslash->next) {
		if (loadslash->name) {
			// these are half-loaded slash channels and save in the same way
			fprintf(fl, "Slash-channel: %s\n", loadslash->name);
		}
	}
	if (SYSLOG_FLAGS(ch)) {
		fprintf(fl, "Syslog Flags: %s\n", bitv_to_alpha(SYSLOG_FLAGS(ch)));
	}
	
	// 'T'
	if (GET_TITLE(ch)) {
		fprintf(fl, "Title: %s\n", GET_TITLE(ch));
	}
	if (GET_TOMB_ROOM(ch) != NOWHERE) {
		fprintf(fl, "Tomb Room: %d\n", GET_TOMB_ROOM(ch));
	}
	
	// 'U'
	if (USING_POISON(ch)) {
		fprintf(fl, "Using Poison: %d\n", USING_POISON(ch));
	}
	
	// END TAGS
	fprintf(fl, "End\n");
	
	// re-apply: affects
	for (af = af_list; af; af = next_af) {
		next_af = af->next;
		affect_to_char(ch, af);
		free(af);
	}
	
	// re-apply: equipment
	for (iter = 0; iter < NUM_WEARS; ++iter) {
		if (char_eq[iter]) {
			#ifndef NO_EXTRANEOUS_TRIGGERS
				if (wear_otrigger(char_eq[iter], ch, iter)) {
			#endif
					// this line may depend on the above if
					equip_char(ch, char_eq[iter], iter);
			#ifndef NO_EXTRANEOUS_TRIGGERS
				}
				else {
					obj_to_char(char_eq[iter], ch);
				}
			#endif
		}
	}
	
	// affect_total(ch); // unnecessary, I think (?)
}


 //////////////////////////////////////////////////////////////////////////////
//// HELPERS /////////////////////////////////////////////////////////////////

/**
* Clears certain player data, similar to clear_char() -- but not for NPCS.
*
* @param char_data *ch The player charater to clear.
*/
void clear_player(char_data *ch) {
	if (!ch) {
		return;
	}
	
	ch->player.time.birth = time(0);
	ch->player.time.played = 0;
	ch->player.time.logon = time(0);
	
	
	// some nowheres/nothings
	GET_LOADROOM(ch) = NOWHERE;
	GET_MOUNT_VNUM(ch) = NOTHING;
	GET_EMPIRE_VNUM(ch) = NOTHING;
	GET_PLEDGE(ch) = NOTHING;
	GET_TOMB_ROOM(ch) = NOWHERE;
	GET_ADVENTURE_SUMMON_RETURN_LOCATION(ch) = NOWHERE;
	GET_ADVENTURE_SUMMON_RETURN_MAP(ch) = NOWHERE;
	GET_LAST_TELL(ch) = NOBODY;
	GET_TEMPORARY_ACCOUNT_ID(ch) = NOTHING;
}


/**
* Function to DELETE A PLAYER.
*
* @param char_data *ch The player to delete.
*/
void delete_player_character(char_data *ch) {
	void clear_private_owner(int id);
	void Crash_delete_file(char *name);
	
	player_index_data *index;
	empire_data *emp = NULL;
	char filename[256];
	
	if (IS_NPC(ch)) {
		syslog(SYS_ERROR, 0, TRUE, "SYSERR: delete_player_character called on NPC");
		return;
	}
	
	clear_private_owner(GET_IDNUM(ch));

	// Check the empire
	if ((emp = GET_LOYALTY(ch)) != NULL) {
		GET_LOYALTY(ch) = NULL;
		GET_EMPIRE_VNUM(ch) = NOTHING;
		GET_RANK(ch) = 0;
	}
	
	// remove account and player index
	if (GET_ACCOUNT(ch)) {
		remove_player_from_account(ch);
	}
	if ((index = find_player_index_by_idnum(GET_IDNUM(ch)))) {
		remove_player_from_table(index);
	}
	
	// various file deletes
	Crash_delete_file(GET_NAME(ch));
	delete_variables(GET_NAME(ch));
	if (get_filename(GET_NAME(ch), filename, ALIAS_FILE)) {
		if (remove(filename) < 0 && errno != ENOENT) {
			log("SYSERR: deleting alias file %s: %s", filename, strerror(errno));
		}
	}
	if (get_filename(GET_NAME(ch), filename, PLR_FILE)) {
		if (remove(filename) < 0 && errno != ENOENT) {
			log("SYSERR: deleting player file %s: %s", filename, strerror(errno));
		}
	}
	
	// cleanup
	if (emp) {
		read_empire_members(emp, FALSE);
	}
}


/**
* Does various checks and puts the player into the game. Both return codes are
* successful results.
*
* @param descriptor_data *d The descriptor for the player.
* @param int dolog Whether or not to log the login (passed to Objload_char).
* @param bool fresh If FALSE, player was already in the game, not logging in fresh.
* @return int 1 = rent-saved, 0 = crash-saved
*/
int enter_player_game(descriptor_data *d, int dolog, bool fresh) {
	void assign_class_abilities(char_data *ch, int class, int role);
	void clean_lore(char_data *ch);
	extern int Objload_char(char_data *ch, int dolog);
	void read_aliases(char_data *ch);
	extern room_data *find_home(char_data *ch);
	extern room_data *find_load_room(char_data *ch);
	void read_saved_vars(char_data *ch);
	
	extern bool global_mute_slash_channel_joins;

	struct slash_channel *load_slash, *next_slash;
	room_data *load_room = NULL, *map_loc;
	char_data *ch = d->character;
	char lbuf[MAX_STRING_LENGTH];
	player_index_data *index;
	bool try_home = FALSE;
	empire_data *emp;
	int load_result;
	int iter;

	reset_char(ch);
	read_aliases(ch);
	
	// remove this now
	REMOVE_BIT(PLR_FLAGS(ch), PLR_KEEP_LAST_LOGIN_INFO);
	
	// ensure they have this
	if (!GET_CREATION_HOST(ch) || !*GET_CREATION_HOST(ch)) {
		if (GET_CREATION_HOST(ch)) {
			free(GET_CREATION_HOST(ch));
		}
		GET_CREATION_HOST(ch) = str_dup(d->host);
	}
	
	// ensure the player has an idnum and is in the index
	if (GET_IDNUM(ch) <= 0 || !(index = find_player_index_by_idnum(GET_IDNUM(ch)))) {
		GET_IDNUM(ch) = ++top_idnum;
		CREATE(index, player_index_data, 1);
		update_player_index(index, ch);
		add_player_to_table(index);
	}
	
	if (GET_IMMORTAL_LEVEL(ch) > -1) {
		GET_ACCESS_LEVEL(ch) = LVL_TOP - GET_IMMORTAL_LEVEL(ch);
	}
	
	if (PLR_FLAGGED(ch, PLR_INVSTART))
		GET_INVIS_LEV(ch) = GET_ACCESS_LEVEL(ch);

	/*
	 * We have to place the character in a room before equipping them
	 * or equip_char() will gripe about the person in no-where.
	 */
	if (GET_LOADROOM(ch) != NOWHERE) {
		load_room = real_room(GET_LOADROOM(ch));
		
		// this verifies they are still in the same map location as where they logged out
		if (load_room && !PLR_FLAGGED(ch, PLR_LOADROOM)) {
			map_loc = get_map_location_for(load_room);
			if (GET_LOAD_ROOM_CHECK(ch) == NOWHERE || !map_loc || GET_ROOM_VNUM(map_loc) != GET_LOAD_ROOM_CHECK(ch)) {
				// ensure they are on the same continent they used to be when it finds them a new loadroom
				GET_LAST_ROOM(ch) = GET_LOAD_ROOM_CHECK(ch);
				load_room = NULL;
			}
		}
	}
	
	// cancel detected loadroom?
	if (load_room && RESTORE_ON_LOGIN(ch) && PRF_FLAGGED(ch, PRF_AUTORECALL)) {
		load_room = NULL;
		try_home = TRUE;
	}
		
	// long logout and in somewhere hostile
	if (load_room && RESTORE_ON_LOGIN(ch) && ROOM_OWNER(load_room) && empire_is_hostile(ROOM_OWNER(load_room), GET_LOYALTY(ch), load_room)) {
		load_room = NULL;	// re-detect
		try_home = TRUE;
	}
	
	// on request, try to send them home
	if (try_home) {
		load_room = find_home(ch);
	}

	// nowhere found? must detect load room
	if (!load_room) {
		load_room = find_load_room(d->character);
	}

	// absolute failsafe
	if (!load_room) {
		load_room = real_room(0);
	}


	/* fail-safe */
	if (IS_VAMPIRE(ch) && GET_APPARENT_AGE(ch) <= 0)
		GET_APPARENT_AGE(ch) = 25;

	ch->next = character_list;
	character_list = ch;
	char_to_room(ch, load_room);
	load_result = Objload_char(ch, dolog);

	affect_total(ch);
	SAVE_CHAR(ch);
		
	// verify class and skill/gear levels are up-to-date
	update_class(ch);
	determine_gear_level(ch);
	
	// clear some player special data
	GET_MARK_LOCATION(ch) = NOWHERE;

	// re-join slash-channels
	global_mute_slash_channel_joins = TRUE;
	for (load_slash = LOAD_SLASH_CHANNELS(ch); load_slash; load_slash = next_slash) {
		next_slash = load_slash->next;
		if (load_slash->name && *load_slash->name) {
			sprintf(lbuf, "join %s", load_slash->name);
			do_slash_channel(ch, lbuf, 0, 0);
		}
		if (load_slash->name) {
			free(load_slash->name);
		}
		free(load_slash);
	}
	global_mute_slash_channel_joins = FALSE;
	
	// free reset?
	if (RESTORE_ON_LOGIN(ch)) {
		GET_HEALTH(ch) = GET_MAX_HEALTH(ch);
		GET_MOVE(ch) = GET_MAX_MOVE(ch);
		GET_MANA(ch) = GET_MAX_MANA(ch);
		GET_COND(ch, FULL) = MIN(0, GET_COND(ch, FULL));
		GET_COND(ch, THIRST) = MIN(0, GET_COND(ch, THIRST));
		GET_COND(ch, DRUNK) = MIN(0, GET_COND(ch, DRUNK));
		GET_BLOOD(ch) = GET_MAX_BLOOD(ch);
		
		// clear deficits
		for (iter = 0; iter < NUM_POOLS; ++iter) {
			GET_DEFICIT(ch, iter) = 0;
		}
		
		// check for confusion!
		GET_CONFUSED_DIR(ch) = number(0, NUM_SIMPLE_DIRS-1);
		
		// clear this for long logout
		GET_RECENT_DEATH_COUNT(ch) = 0;
		affect_from_char(ch, ATYPE_DEATH_PENALTY);
		
		RESTORE_ON_LOGIN(ch) = FALSE;
		clean_lore(ch);
	}
	else {
		// ensure not dead
		GET_HEALTH(ch) = MAX(1, GET_HEALTH(ch));
		GET_BLOOD(ch) = MAX(1, GET_BLOOD(ch));
	}
	
	// position must be reset
	if (AFF_FLAGGED(ch, AFF_EARTHMELD | AFF_MUMMIFY | AFF_DEATHSHROUD)) {
		GET_POS(ch) = POS_SLEEPING;
	}
	else {
		GET_POS(ch) = POS_STANDING;
	}
	
	// in some cases, we need to re-read tech when the character logs in
	if ((emp = GET_LOYALTY(ch))) {
		if (REREAD_EMPIRE_TECH_ON_LOGIN(ch)) {
			SAVE_CHAR(ch);
			reread_empire_tech(emp);
			REREAD_EMPIRE_TECH_ON_LOGIN(ch) = FALSE;
		}
		else {
			// reread members to adjust greatness
			read_empire_members(emp, FALSE);
		}
	}
	
	// remove stale coins
	cleanup_coins(ch);
	
	// verify abils -- TODO should this remove/re-add abilities for the empire? do class abilities affect that?
	assign_class_abilities(ch, NOTHING, NOTHING);
	
	// ensure player has penalty if at war
	if (fresh && GET_LOYALTY(ch) && is_at_war(GET_LOYALTY(ch))) {
		int duration = config_get_int("war_login_delay") / SECS_PER_REAL_UPDATE;
		struct affected_type *af = create_flag_aff(ATYPE_WAR_DELAY, duration, AFF_IMMUNE_PHYSICAL | AFF_NO_ATTACK | AFF_STUNNED, ch);
		affect_join(ch, af, ADD_DURATION);
	}

	// script/trigger stuff
	GET_ID(ch) = GET_IDNUM(ch);
	read_saved_vars(ch);
	greet_mtrigger(ch, NO_DIR);
	greet_memory_mtrigger(ch);
	add_to_lookup_table(GET_ID(ch), (void *)ch);
	
	// update the index in case any of this changed
	ch->prev_logon = ch->player.time.logon;	// and update prev_logon here
	index = find_player_index_by_idnum(GET_IDNUM(ch));
	update_player_index(index, ch);
	
	return load_result;
}


/**
* This runs one-time setup on the player character, during their initial
* creation.
*
* @param char_data *ch The player to initialize.
*/
void init_player(char_data *ch) {
	extern const int base_player_pools[NUM_POOLS];
	extern const char *syslog_types[];
	
	player_index_data *index;
	int account_id = NOTHING;
	account_data *acct;
	int i, iter;

	// create a player_special structure, if needed
	if (ch->player_specials == NULL) {
		CREATE(ch->player_specials, struct player_special_data, 1);
	}
	
	// store temporary account id (may be overwritten by clear_player)
	if (GET_TEMPORARY_ACCOUNT_ID(ch) != NOTHING) {
		account_id = GET_TEMPORARY_ACCOUNT_ID(ch);
		GET_TEMPORARY_ACCOUNT_ID(ch) = NOTHING;
	}
	
	// some basic player inits
	clear_player(ch);

	GET_IMMORTAL_LEVEL(ch) = -1;				/* Not an immortal */

	/* *** if this is our first player --- he be God *** */
	if (HASH_CNT(idnum_hh, player_table_by_idnum) == 0) {
		GET_ACCESS_LEVEL(ch) = LVL_TOP;
		GET_IMMORTAL_LEVEL(ch) = 0;			/* Initialize it to 0, meaning Implementor */

		ch->real_attributes[STRENGTH] = att_max(ch);
		ch->real_attributes[DEXTERITY] = att_max(ch);

		ch->real_attributes[CHARISMA] = att_max(ch);
		ch->real_attributes[GREATNESS] = att_max(ch);

		ch->real_attributes[INTELLIGENCE] = att_max(ch);
		ch->real_attributes[WITS] = att_max(ch);
		
		SET_BIT(PRF_FLAGS(ch), PRF_HOLYLIGHT | PRF_ROOMFLAGS | PRF_NOHASSLE);
		
		// turn on all syslogs
		for (iter = 0; *syslog_types[iter] != '\n'; ++iter) {
			SYSLOG_FLAGS(ch) |= BIT(iter);
		}
	}
	
	ch->points.max_pools[HEALTH] = base_player_pools[HEALTH];
	ch->points.current_pools[HEALTH] = GET_MAX_HEALTH(ch);
	
	ch->points.max_pools[MOVE] = base_player_pools[MOVE];
	ch->points.current_pools[MOVE] = GET_MAX_MOVE(ch);
	
	ch->points.max_pools[MANA] = base_player_pools[MANA];
	ch->points.current_pools[MANA] = GET_MAX_MANA(ch);
	
	set_title(ch, NULL);
	ch->player.short_descr = NULL;
	ch->player.long_descr = NULL;
	GET_PROMPT(ch) = NULL;
	GET_FIGHT_PROMPT(ch) = NULL;
	POOFIN(ch) = NULL;
	POOFOUT(ch) = NULL;
	
	ch->points.max_pools[BLOOD] = 10;	// not actually used for players
	ch->points.current_pools[BLOOD] = GET_MAX_BLOOD(ch);	// this is a function
	
	// assign idnum
	if (GET_IDNUM(ch) <= 0 || !(index = find_player_index_by_idnum(GET_IDNUM(ch)))) {
		GET_IDNUM(ch) = ++top_idnum;
		CREATE(index, player_index_data, 1);
		update_player_index(index, ch);
		add_player_to_table(index);
	}
	// assign account
	if (account_id != NOTHING || !GET_ACCOUNT(ch)) {
		if ((acct = find_account(account_id))) {
			if (GET_ACCOUNT(ch)) {
				remove_player_from_account(ch);
			}
			add_player_to_account(ch, acct);
		}
		else if (!GET_ACCOUNT(ch)) {
			create_account_for_player(ch);
		}
	}
	
	ch->char_specials.saved.affected_by = 0;

	for (i = 0; i < NUM_CONDS; i++)
		GET_COND(ch, i) = (GET_ACCESS_LEVEL(ch) == LVL_IMPL ? UNLIMITED : 0);
}


/* clear some of the the working variables of a char */
void reset_char(char_data *ch) {
	int i;

	for (i = 0; i < NUM_WEARS; i++)
		GET_EQ(ch, i) = NULL;

	ch->followers = NULL;
	ch->master = NULL;
	IN_ROOM(ch) = NULL;
	ch->carrying = NULL;
	ch->next = NULL;
	ch->next_fighting = NULL;
	ch->next_in_room = NULL;
	ON_CHAIR(ch) = NULL;
	FIGHTING(ch) = NULL;
	ch->char_specials.position = POS_STANDING;
	ch->char_specials.carry_items = 0;

	if (GET_MOVE(ch) <= 0) {
		GET_MOVE(ch) = 1;
	}
}


/**
* This handles title-setting to allow some characters (, - ; : ~) to appear
* in the title with no leading space.
*
* @param char_data *ch The player to set.
* @param char *title The title string to set.
*/
void set_title(char_data *ch, char *title) {
	char buf[MAX_STRING_LENGTH];
	
	if (IS_NPC(ch)) {
		return;
	}

	if (title == NULL)
		title = "the newbie";

	if (GET_TITLE(ch) != NULL)
		free(GET_TITLE(ch));

	if (*title != ':' && *title != ',' && *title != '-' && *title != ';' && *title != '~')
		sprintf(buf, " %s", title);
	else
		strcpy(buf, title);

	if (strlen(title) > MAX_TITLE_LENGTH) {
		buf[MAX_TITLE_LENGTH] = '\0';
	}

	GET_TITLE(ch) = str_dup(buf);
}


/**
* Some initializations for characters, including initial skills
*
* @param char_data *ch A new player
*/
void start_new_character(char_data *ch) {
	void apply_bonus_trait(char_data *ch, bitvector_t trait, bool add);
	extern const struct archetype_type archetype[];
	void make_vampire(char_data *ch, bool lore);
	void scale_item_to_level(obj_data *obj, int level);
	void set_skill(char_data *ch, int skill, int level);
	extern const char *default_channels[];
	extern bool global_mute_slash_channel_joins;
	extern struct promo_code_list promo_codes[];
	extern int tips_of_the_day_size;
	
	char lbuf[MAX_INPUT_LENGTH];
	int type, iter;
	obj_data *obj;
	
	// announce to existing players that we have a newbie
	mortlog("%s has joined the game", PERS(ch, ch, TRUE));

	set_title(ch, NULL);

	/* Default Flags */
	SET_BIT(PRF_FLAGS(ch), PRF_MORTLOG);
	if (config_get_bool("siteok_everyone")) {
		SET_BIT(PLR_FLAGS(ch), PLR_SITEOK);
	}
	
	// not sure how they could not have this...
	if (ch->desc) {
		if (GET_CREATION_HOST(ch)) {
			free(GET_CREATION_HOST(ch));
		}
		GET_CREATION_HOST(ch) = str_dup(ch->desc->host);
	}
	else {
		GET_CREATION_HOST(ch) = NULL;
	}

	// level
	if (GET_ACCESS_LEVEL(ch) < LVL_APPROVED && !config_get_bool("require_auth")) {
		GET_ACCESS_LEVEL(ch) = LVL_APPROVED;
	}

	GET_HEALTH(ch) = GET_MAX_HEALTH(ch);
	GET_MOVE(ch) = GET_MAX_MOVE(ch);
	GET_MANA(ch) = GET_MAX_MANA(ch);
	GET_BLOOD(ch) = GET_MAX_BLOOD(ch);

	/* Standard conditions */
	GET_COND(ch, THIRST) = 0;
	GET_COND(ch, FULL) = 0;
	GET_COND(ch, DRUNK) = 0;

	// base stats: min 1
	for (iter = 0; iter < NUM_ATTRIBUTES; ++iter) {
		GET_REAL_ATT(ch, iter) = MAX(1, GET_REAL_ATT(ch, iter));
	}
	
	// randomize first tip
	GET_LAST_TIP(ch) = number(0, tips_of_the_day_size - 1);
	
	// confuuuusing
	GET_CONFUSED_DIR(ch) = number(0, NUM_SIMPLE_DIRS-1);

	/* Start playtime */
	ch->player.time.played = 0;
	ch->player.time.logon = time(0);
	
	SET_BIT(PRF_FLAGS(ch), PRF_AUTOKILL);
	
	// ensure custom channel colors default to off
	for (iter = 0; iter < NUM_CUSTOM_COLORS; ++iter) {
		GET_CUSTOM_COLOR(ch, iter) = 0;
	}
	
	// add the default slash channels
	global_mute_slash_channel_joins = TRUE;
	for (iter = 0; *default_channels[iter] != '\n'; ++iter) {
		sprintf(lbuf, "join %s", default_channels[iter]);
		do_slash_channel(ch, lbuf, 0, 0);
	}
	global_mute_slash_channel_joins = FALSE;

	/* Give EQ, if applicable */
	if (CREATION_ARCHETYPE(ch) != 0) {
		type = CREATION_ARCHETYPE(ch);
		
		// attributes
		for (iter = 0; iter < NUM_ATTRIBUTES; ++iter) {
			ch->real_attributes[iter] = archetype[type].attributes[iter];
		}
	
		// skills
		if (archetype[type].primary_skill != NO_SKILL && GET_SKILL(ch, archetype[type].primary_skill) < archetype[type].primary_skill_level) {
			set_skill(ch, archetype[type].primary_skill, archetype[type].primary_skill_level);
		}
		if (archetype[type].secondary_skill != NO_SKILL && GET_SKILL(ch, archetype[type].secondary_skill) < archetype[type].secondary_skill_level) {
			set_skill(ch, archetype[type].secondary_skill, archetype[type].secondary_skill_level);
		}
		
		// vampire?
		if (!IS_VAMPIRE(ch) && (archetype[type].primary_skill == SKILL_VAMPIRE || archetype[type].secondary_skill == SKILL_VAMPIRE)) {
			make_vampire(ch, TRUE);
			GET_BLOOD(ch) = GET_MAX_BLOOD(ch);
		}
		
		// newbie eq -- don't run load triggers on this set, as ch is possibly not in a room
		for (iter = 0; archetype[type].gear[iter].vnum != NOTHING; ++iter) {
			// skip slots that are somehow full
			if (archetype[type].gear[iter].wear != NOWHERE && GET_EQ(ch, archetype[type].gear[iter].wear) != NULL) {
				continue;
			}
			
			obj = read_object(archetype[type].gear[iter].vnum, TRUE);
			scale_item_to_level(obj, 1);	// lowest possible scale
			
			if (archetype[type].gear[iter].wear == NOWHERE) {
				obj_to_char(obj, ch);
			}
			else {
				equip_char(ch, obj, archetype[type].gear[iter].wear);
			}
		}

		// misc items
		obj = read_object(o_GRAVE_MARKER, TRUE);
		scale_item_to_level(obj, 1);	// lowest possible scale
		obj_to_char(obj, ch);
		
		for (iter = 0; iter < 2; ++iter) {
			// 2 bread
			obj = read_object(o_BREAD, TRUE);
			scale_item_to_level(obj, 1);	// lowest possible scale
			obj_to_char(obj, ch);
			
			// 2 trinket of conveyance
			obj = read_object(o_TRINKET_OF_CONVEYANCE, TRUE);
			scale_item_to_level(obj, 1);	// lowest possible scale
			obj_to_char(obj, ch);
		}
		
		obj = read_object(o_BOWL, TRUE);
		scale_item_to_level(obj, 1);	// lowest possible scale
		GET_OBJ_VAL(obj, VAL_DRINK_CONTAINER_CONTENTS) = GET_DRINK_CONTAINER_CAPACITY(obj);
		GET_OBJ_VAL(obj, VAL_DRINK_CONTAINER_TYPE) = LIQ_WATER;
		obj_to_char(obj, ch);
		determine_gear_level(ch);
	}
	
	// apply any bonus traits that needed it
	apply_bonus_trait(ch, GET_BONUS_TRAITS(ch), TRUE);
	
	// if they have a valid promo code, apply it now
	if (GET_PROMO_ID(ch) >= 0 && promo_codes[GET_PROMO_ID(ch)].apply_func) {
		(promo_codes[GET_PROMO_ID(ch)].apply_func)(ch);
	}
	
	// set up class/level data
	update_class(ch);
	
	// prevent a repeat
	REMOVE_BIT(PLR_FLAGS(ch), PLR_NEEDS_NEWBIE_SETUP);
}


 //////////////////////////////////////////////////////////////////////////////
//// EMPIRE PLAYER MANAGEMENT ////////////////////////////////////////////////

// this is used to ensure each account only contributes once to the empire
struct empire_member_reader_data {
	empire_data *empire;
	int account_id;
	int greatness;
	
	struct empire_member_reader_data *next;
};


/**
* Add a given user's data to the account list of accounts on the empire member reader data
*
* @param struct empire_member_reader_data **list A pointer to the existing list.
* @param int empire which empire entry the player is in
* @param int account_id which account id the player has
* @param int greatness the player's greatness
*/
static void add_to_account_list(struct empire_member_reader_data **list, empire_data *empire, int account_id, int greatness) {
	struct empire_member_reader_data *emrd;
	bool found = FALSE;
	
	for (emrd = *list; emrd && !found; emrd = emrd->next) {
		if (emrd->empire == empire && emrd->account_id == account_id) {
			found = TRUE;
			emrd->greatness = MAX(emrd->greatness, greatness);
		}
	}
	
	if (!found) {
		CREATE(emrd, struct empire_member_reader_data, 1);
		emrd->empire = empire;
		emrd->account_id = account_id;
		emrd->greatness = greatness;
		
		emrd->next = *list;
		*list = emrd;
	}
}


/**
* Determines is an empire member is timed out based on his playtime, creation
* time, and last login.
*
* @param time_t created The player character's birth time.
* @param time_t last_login The player's last login time.
* @param double played_hours Number of hours the player has played, total.
* @return bool TRUE if the member has timed out and should not be counted; FALSE if they're ok.
*/
static bool member_is_timed_out(time_t created, time_t last_login, double played_hours) {
	int member_timeout_full = config_get_int("member_timeout_full") * SECS_PER_REAL_DAY;
	int member_timeout_newbie = config_get_int("member_timeout_newbie") * SECS_PER_REAL_DAY;
	int minutes_per_day_full = config_get_int("minutes_per_day_full");
	int minutes_per_day_newbie = config_get_int("minutes_per_day_newbie");

	if (played_hours >= config_get_int("member_timeout_max_threshold")) {
		return (last_login + member_timeout_full) < time(0);
	}
	else {
		double days_played = (double)(time(0) - created) / SECS_PER_REAL_DAY;
		double avg_min_per_day = 60 * (played_hours / days_played);
		double timeout;
		
		// when playtime drops this low, the character is ALWAYS timed out
		if (avg_min_per_day <= 1) {
			return TRUE;
		}
		
		if (avg_min_per_day >= minutes_per_day_full) {
			timeout = member_timeout_full;
		}
		else if (avg_min_per_day <= minutes_per_day_newbie) {
			timeout = member_timeout_newbie;
		}
		else {
			// somewhere in the middle
			double prc = (avg_min_per_day - minutes_per_day_newbie) / (minutes_per_day_full - minutes_per_day_newbie);
			double scale = member_timeout_full - member_timeout_newbie;
			timeout = member_timeout_newbie + (prc * scale);
		}
		
		return (last_login + timeout) < time(0);
	}
}


/**
* Calls member_is_timed_out() using a player_index_data.
*
* @param player_index_data *index A pointer to the playertable entry.
* @return bool TRUE if the member has timed out and should not be counted; FALSE if they're ok.
*/
bool member_is_timed_out_index(player_index_data *index) {
	return member_is_timed_out(index->birth, index->last_logon, ((double)index->played) / SECS_PER_REAL_HOUR);
}


/**
* Calls member_is_timed_out() using a char_data.
*
* @param char_data *ch A character to compute timeout on.
* @return bool TRUE if the member has timed out and should not be counted; FALSE if they're ok.
*/
bool member_is_timed_out_ch(char_data *ch) {
	return member_is_timed_out(ch->player.time.birth, ch->player.time.logon, ((double)ch->player.time.played) / SECS_PER_REAL_HOUR);
}


/**
* This function reads and re-sets member-realted aspects of all empires,
* but it does not clear technology flags before adding in new ones -- if you
* need to do that, call reread_empire_tech() instead.
*
* @param int only_empire if not NOTHING, only reads 1 empire
* @param bool read_techs if TRUE, will add techs based on players (usually only during startup)
*/
void read_empire_members(empire_data *only_empire, bool read_techs) {
	void Objsave_char(char_data *ch, int rent_code);
	extern int Objload_char(char_data *ch, int dolog);
	void resort_empires();
	bool should_delete_empire(empire_data *emp);
	
	struct empire_member_reader_data *account_list = NULL, *emrd;
	player_index_data *index, *next_index;
	empire_data *e, *emp, *next_emp;
	char_data *ch;
	time_t logon;
	bool is_file;

	HASH_ITER(hh, empire_table, emp, next_emp) {
		if (!only_empire || emp == only_empire) {
			EMPIRE_TOTAL_MEMBER_COUNT(emp) = 0;
			EMPIRE_MEMBERS(emp) = 0;
			EMPIRE_GREATNESS(emp) = 0;
			EMPIRE_TOTAL_PLAYTIME(emp) = 0;
			EMPIRE_LAST_LOGON(emp) = 0;
		}
	}
	
	HASH_ITER(idnum_hh, player_table_by_idnum, index, next_index) {
		if (only_empire && index->loyalty != only_empire) {
			continue;
		}
		
		// new way of loading data
		if ((ch = find_or_load_player(index->name, &is_file))) {
			affect_total(ch);
			
			if (is_file) {
				Objload_char(ch, FALSE);
			}
		}
		
		// check ch for empire traits
		if (ch && (e = GET_LOYALTY(ch))) {
			// record last-logon whether or not timed out
			logon = is_file ? ch->prev_logon : time(0);
			if (logon > EMPIRE_LAST_LOGON(e)) {
				EMPIRE_LAST_LOGON(e) = logon;
			}

			if (GET_ACCESS_LEVEL(ch) >= LVL_GOD) {
				EMPIRE_IMM_ONLY(e) = 1;
			}
			
			// always
			EMPIRE_TOTAL_MEMBER_COUNT(e) += 1;
			
			// only count players who have logged on in recent history
			if (!member_is_timed_out_ch(ch)) {
				add_to_account_list(&account_list, e, GET_ACCOUNT(ch)->id, GET_GREATNESS(ch));
				
				// not account-restricted
				EMPIRE_TOTAL_PLAYTIME(e) += (ch->player.time.played / SECS_PER_REAL_HOUR);

				if (read_techs) {
					adjust_abilities_to_empire(ch, e, TRUE);
				}
			}
		}
		
		if (ch && is_file) {
			free_char(ch);
		}
	}
	
	// now apply the best from each account, and clear out the list
	while ((emrd = account_list)) {
		EMPIRE_MEMBERS(emrd->empire) += 1;
		EMPIRE_GREATNESS(emrd->empire) += emrd->greatness;
		
		account_list = emrd->next;
		free(emrd);
	}
	
	// delete emptypires
	HASH_ITER(hh, empire_table, emp, next_emp) {
		if ((!only_empire || emp == only_empire) && should_delete_empire(emp)) {
			delete_empire(emp);
			
			// don't accidentally keep deleting if we were only doing 1 (it's been freed)
			if (only_empire) {
				break;
			}
		}
	}
	
	// re-sort now only if we aren't reading techs (this hints that we're also reading territory)
	if (!read_techs) {
		resort_empires();
	}
}


 //////////////////////////////////////////////////////////////////////////////
//// PROMO CODES /////////////////////////////////////////////////////////////

// these are configured in config.c


// starting coins
PROMO_APPLY(promo_countdemonet) {
	increase_coins(ch, REAL_OTHER_COIN, 100);
}


// bonus charisma
PROMO_APPLY(promo_facebook) {
	ch->real_attributes[CHARISMA] = MAX(1, MIN(att_max(ch), ch->real_attributes[CHARISMA] + 1));
	affect_total(ch);
}


// 1.5x skills
PROMO_APPLY(promo_skillups) {
	int iter;
	
	for (iter = 0; iter < NUM_SKILLS; ++iter) {
		if (GET_SKILL(ch, iter) > 0) {
			set_skill(ch, iter, MIN(BASIC_SKILL_CAP, GET_SKILL(ch, iter) * 1.5));
		}
	}
}
