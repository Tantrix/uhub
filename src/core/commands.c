/*
 * uhub - A tiny ADC p2p connection hub
 * Copyright (C) 2007-2009, Jan Vidar Krey
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "uhub.h"

#ifdef DEBUG
#define CRASH_DEBUG
#endif

#define MAX_HELP_MSG 1024

struct hub_command
{
	const char* message;
	char* prefix;
	size_t prefix_len;
	struct linked_list* args;
};

typedef int (*command_handler)(struct hub_info* hub, struct hub_user* user, struct hub_command*);

struct commands_handler
{
	const char* prefix;
	size_t length;
	const char* args;
	enum user_credentials cred;
	command_handler handler;
	const char* description;
};

static struct commands_handler command_handlers[];

static void command_destroy(struct hub_command* cmd)
{
	if (!cmd) return;
	hub_free(cmd->prefix);

	if (cmd->args)
	{
		list_clear(cmd->args, &hub_free);
		list_destroy(cmd->args);
	}

	hub_free(cmd);
}

static struct hub_command* command_create(const char* message)
{
	struct hub_command* cmd = hub_malloc_zero(sizeof(struct hub_command));
	if (!cmd) return 0;

	cmd->message = message;
	cmd->args = list_create();

	int n = split_string(message, "\\s", cmd->args, 0);
	if (n <= 0)
	{
		command_destroy(cmd);
		return 0;
	}

	char* prefix = list_get_first(cmd->args);
	if (prefix[0] && prefix[1])
	{
		cmd->prefix = hub_strdup(&prefix[1]);
		cmd->prefix_len = strlen(cmd->prefix);
	}
	else
	{
		command_destroy(cmd);
		return 0;
	}
	list_remove(cmd->args, prefix);
	hub_free(prefix);
	return cmd;
}

static void send_message(struct hub_info* hub, struct hub_user* user, const char* message)
{
	char* buffer = adc_msg_escape(message);
	struct adc_message* command = adc_msg_construct(ADC_CMD_IMSG, strlen(buffer) + 6);
	adc_msg_add_argument(command, buffer);
	route_to_user(hub, user, command);
	adc_msg_free(command);
	hub_free(buffer);
}

static int command_access_denied(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char temp[128];
	snprintf(temp, 128, "*** %s: Access denied.", cmd->prefix);
	send_message(hub, user, temp);
	return 0;
}

static int command_not_found(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char temp[128];
	snprintf(temp, 128, "*** %s: Command not found", cmd->prefix);
	send_message(hub, user, temp);
	return 0;
}

static int command_status_user_not_found(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd, const char* nick)
{
	char temp[128];
	snprintf(temp, 128, "*** %s: No user \"%s\"", cmd->prefix, nick);
	send_message(hub, user, temp);
	return 0;
}

const char* command_get_syntax(struct commands_handler* handler)
{
	static char args[128];
	args[0] = 0;
	size_t n = 0;
	if (handler->args)
	{
		for (n = 0; n < strlen(handler->args); n++)
		{
			if (n > 0) strcat(args, " ");
			switch (handler->args[n])
			{
				case 'n': strcat(args, "<nick>"); break;
				case 'c': strcat(args, "<cid>"); break;
				case 'a': strcat(args, "<addr>"); break;
			}
		}
	}
	return args;
}

static int command_arg_mismatch(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd, struct commands_handler* handler)
{
	char temp[256];
	const char* args = command_get_syntax(handler);
	if (args) snprintf(temp, 256, "*** %s: Use: !%s %s", cmd->prefix, cmd->prefix, args);
	else      snprintf(temp, 256, "*** %s: Use: !%s", cmd->prefix, cmd->prefix);
	send_message(hub, user, temp);
	return 0;
}

static int command_status(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd, const char* message)
{
	char temp[1024];
	snprintf(temp, 1024, "*** %s: %s", cmd->prefix, message);
	send_message(hub, user, temp);
	return 0;
}

static int command_stats(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char temp[128];
	snprintf(temp, 128, "%zu users, peak: %zu. Network (up/down): %d/%d KB/s, peak: %d/%d KB/s",
	hub->users->count,
	hub->users->count_peak,
	(int) hub->stats.net_tx / 1024,
	(int) hub->stats.net_rx / 1024,
	(int) hub->stats.net_tx_peak / 1024,
	(int) hub->stats.net_rx_peak / 1024);
	return command_status(hub, user, cmd, temp);
}

static int command_help(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	size_t n;
	char msg[MAX_HELP_MSG];
	msg[0] = 0;
	strcat(msg, "Available commands:\n");

	for (n = 0; command_handlers[n].prefix; n++)
	{
		if (command_handlers[n].cred <= user->credentials)
		{
			strcat(msg, "!");
			strcat(msg, command_handlers[n].prefix);
			strcat(msg, " - ");
			strcat(msg, command_handlers[n].description);
			strcat(msg, "\n");
		}
	}
	return command_status(hub, user, cmd, msg);
}

static int command_uptime(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char tmp[128];
	size_t d;
	size_t h;
	size_t m;
	size_t D = (size_t) difftime(time(0), hub->tm_started);

	d = D / (24 * 3600);
	D = D % (24 * 3600);
	h = D / 3600;
	D = D % 3600;
	m = D / 60;

	tmp[0] = 0;
	if (d)
	{
		strcat(tmp, uhub_itoa((int) d));
		strcat(tmp, " day");
		if (d != 1) strcat(tmp, "s");
		strcat(tmp, ", ");
	}

	if (h < 10) strcat(tmp, "0");
	strcat(tmp, uhub_itoa((int) h));
	strcat(tmp, ":");
	if (m < 10) strcat(tmp, "0");
	strcat(tmp, uhub_itoa((int) m));

	return command_status(hub, user, cmd, tmp);
}

static int command_kick(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char* nick = list_get_first(cmd->args);
	struct hub_user* target = uman_get_user_by_nick(hub, nick);
	
	if (!target)
		return command_status_user_not_found(hub, user, cmd, nick);
	
	if (target == user)
		return command_status(hub, user, cmd, "Cannot kick yourself");
	
	hub_disconnect_user(hub, target, quit_kicked);
	return command_status(hub, user, cmd, nick);
}

static int command_ban(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char* nick = list_get_first(cmd->args);
	struct hub_user* target = uman_get_user_by_nick(hub, nick);

	if (!target)
		return command_status_user_not_found(hub, user, cmd, nick);

	if (target == user)
		return command_status(hub, user, cmd, "Cannot kick/ban yourself");

	hub_disconnect_user(hub, target, quit_kicked);
	acl_user_ban_nick(hub->acl, target->id.nick);
	acl_user_ban_cid(hub->acl, target->id.cid);

	return command_status(hub, user, cmd, nick);
}

static int command_unban(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	return command_status(hub, user, cmd, "Not implemented");
}

static int command_reload(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	hub->status = hub_status_restart;
	return command_status(hub, user, cmd, "Reloading configuration...");
}

static int command_shutdown(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	hub->status = hub_status_shutdown;
	return command_status(hub, user, cmd, "Hub shutting down...");
}

static int command_version(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	return command_status(hub, user, cmd, "Powered by " PRODUCT "/" VERSION);
}

static int command_myip(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char tmp[128];
	snprintf(tmp, 128, "Your address is \"%s\"", ip_convert_to_string(&user->net.ipaddr));
	return command_status(hub, user, cmd, tmp);
}

static int command_getip(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	char tmp[128];

	char* nick = list_get_first(cmd->args);
	struct hub_user* target = uman_get_user_by_nick(hub, nick);

	if (!target)
		return command_status_user_not_found(hub, user, cmd, nick);

	snprintf(tmp, 128, "%s has address \"%s\"", nick, ip_convert_to_string(&target->net.ipaddr));
	return command_status(hub, user, cmd, tmp);
}

#ifdef CRASH_DEBUG
static int command_crash(struct hub_info* hub, struct hub_user* user, struct hub_command* cmd)
{
	void (*crash)(void) = NULL;
	crash();
	return 0;
}
#endif

int command_dipatcher(struct hub_info* hub, struct hub_user* user, const char* message)
{
	size_t n = 0;
	int rc;

	/* Parse and validate the command */
	struct hub_command* cmd = command_create(message);
	if (!cmd) return 1;

	for (n = 0; command_handlers[n].prefix; n++)
	{
		struct commands_handler* handler = &command_handlers[n];
		if (cmd->prefix_len != handler->length)
			continue;

		if (!strncmp(cmd->prefix, handler->prefix, handler->length))
		{
			if (handler->cred <= user->credentials)
			{
				if (!handler->args || (handler->args && list_size(cmd->args) >= strlen(handler->args)))
				{
					rc = handler->handler(hub, user, cmd);
				}
				else
				{
					rc = command_arg_mismatch(hub, user, cmd, handler);
				}
				command_destroy(cmd);
				return rc;
			}
			else
			{
				rc = command_access_denied(hub, user, cmd);
				command_destroy(cmd);
				return rc;
			}
		}
	}

	command_not_found(hub, user, cmd);
	command_destroy(cmd);
	return 1;
}

static struct commands_handler command_handlers[] = {
	{ "help",       4, 0,   cred_guest,     command_help,     "Show this help message."      },
	{ "stats",      5, 0,   cred_super,     command_stats,    "Show hub statistics."         },
	{ "version",    7, 0,   cred_guest,     command_version,  "Show hub version info."       },
	{ "uptime",     6, 0,   cred_guest,     command_uptime,   "Display hub uptime info."     },
	{ "kick",       4, "n", cred_operator,  command_kick,     "Kick a user"                  },
	{ "ban",        3, "n", cred_operator,  command_ban,      "Ban a user"                   },
	{ "unban",      5, "n", cred_operator,  command_unban,    "Lift ban on a user"           },
	{ "reload",     6, 0,   cred_admin,     command_reload,   "Reload configuration files."  },
	{ "shutdown",   8, 0,   cred_admin,     command_shutdown, "Shutdown hub."                },
	{ "myip",       4, 0,   cred_guest,     command_myip,     "Show your own IP."            },
	{ "getip",      5, "n", cred_operator,  command_getip,    "Show IP address for a user"   },
#ifdef CRASH_DEBUG
	{ "crash",      5, 0,   cred_admin,     command_crash,    "Crash the hub (DEBUG)."       },
#endif
	{ 0,            0, 0,   cred_none,      command_help,     ""                             }
};
