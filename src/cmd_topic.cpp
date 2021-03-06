/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *	     	          E-mail:
 *                <brain@chatspike.net>
 *                <Craig@chatspike.net>
 *
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd_config.h"
#include "configreader.h"
#include "users.h"
#include "modules.h"
#include "commands.h"
#include "commands/cmd_topic.h"


void cmd_topic::Handle (const char** parameters, int pcnt, userrec *user)
{
	chanrec* Ptr;

	if (pcnt == 1)
	{
		Ptr = ServerInstance->FindChan(parameters[0]);
		if (Ptr)
		{
			if ((Ptr->modes[CM_SECRET]) && (!Ptr->HasUser(user)))
			{
				user->WriteServ("401 %s %s :No such nick/channel",user->nick, Ptr->name);
				return;
			}
			if (Ptr->topicset)
			{
				user->WriteServ("332 %s %s :%s", user->nick, Ptr->name, Ptr->topic);
				user->WriteServ("333 %s %s %s %d", user->nick, Ptr->name, Ptr->setby, Ptr->topicset);
			}
			else
			{
				user->WriteServ("331 %s %s :No topic is set.", user->nick, Ptr->name);
			}
		}
		else
		{
			user->WriteServ("401 %s %s :No such nick/channel",user->nick, parameters[0]);
		}
		return;
	}
	else if (pcnt>1)
	{
		Ptr = ServerInstance->FindChan(parameters[0]);
		if (Ptr)
		{
			if (IS_LOCAL(user))
			{
				if (!Ptr->HasUser(user))
				{
					user->WriteServ("442 %s %s :You're not on that channel!",user->nick, Ptr->name);
					return;
				}
				if ((Ptr->modes[CM_TOPICLOCK]) && (Ptr->GetStatus(user) < STATUS_HOP))
				{
					user->WriteServ("482 %s %s :You must be at least a half-operator to change modes on this channel", user->nick, Ptr->name);
					return;
				}
			}
			char topic[MAXTOPIC];
			strlcpy(topic,parameters[1],MAXTOPIC-1);

			if (IS_LOCAL(user))
			{
				int MOD_RESULT = 0;
				FOREACH_RESULT(I_OnLocalTopicChange,OnLocalTopicChange(user,Ptr,topic));
				if (MOD_RESULT)
					return;
			}

			strlcpy(Ptr->topic,topic,MAXTOPIC-1);
			strlcpy(Ptr->setby,user->nick,NICKMAX-1);
			Ptr->topicset = ServerInstance->Time();
			Ptr->WriteChannel(user, "TOPIC %s :%s", Ptr->name, Ptr->topic);
			if (IS_LOCAL(user))
			{
				FOREACH_MOD(I_OnPostLocalTopicChange,OnPostLocalTopicChange(user,Ptr,topic));
			}
		}
		else
		{
			user->WriteServ("401 %s %s :No such nick/channel",user->nick, parameters[0]);
		}
	}
}

