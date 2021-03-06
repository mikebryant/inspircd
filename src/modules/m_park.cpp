/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

using namespace std;

#include <stdio.h>
#include <string>
#include <vector>
#include "users.h"
#include "channels.h"

#include "modules.h"
#include "inspircd.h"



/* $ModDesc: Provides support for user parking/unparking */

class parking : public classbase
{
};

class awaymsg : public parking
{
 public:
	std::string from;
	std::string text;
	time_t tm;
};

class parkedinfo : public parking
{
 public:
	std::string nick;
	std::string host;
	time_t parktime;
};


typedef std::vector<awaymsg> awaylog;
typedef std::vector<parkedinfo> parkinfo;
parkinfo pinfo;
long ParkMaxTime;
long ConcurrentParks;
long ParkMaxMsgs;
parkedinfo pi;

class cmd_park : public command_t
{
 public:
 cmd_park (InspIRCd* Instance) : command_t(Instance,"PARK", 0, 0)
	{
		this->source = "m_park.so";
	}

	void Handle (const char** parameters, int pcnt, userrec *user)
	{
		/** Parking. easy stuff.
		 *
		 * We duplicate and switch the users file descriptor, so that they can remain forever as a 'ghost'
		 * We then disconnect the real user leaving a controlled ghost in their place :)
		 */
		int othersessions = 0;
		/* XXX - why can't just use pinfo.size() like we do below, rather than iterating over the whole vector? -- w00t */
		if (pinfo.size())
			for (parkinfo::iterator j = pinfo.begin(); j != pinfo.end(); j++)
				if (j->host == std::string(user->host))
					othersessions++;
		if (othersessions >= ConcurrentParks)
		{
			user->WriteServ("927 "+std::string(user->nick)+" :You are already parked up to the maximum number of allowed times.");
		}
		else
		{
			awaylog* aw;
			char msg[MAXBUF];
			unsigned long* key = new unsigned long;
			*key = abs(random() * 12345);
			snprintf(msg,MAXBUF,"You are now parked. To unpark use /UNPARK %s %lu",user->nick, *key);
			ServerInstance->UserToPseudo(user,std::string(msg));
			aw = new awaylog;
			user->Extend("park_awaylog", aw);
			user->Extend("park_key", key);
			pi.nick = user->nick;
			pi.host = user->host;
			pi.parktime = time(NULL);
			pinfo.push_back(pi);
		}
	}

};

class cmd_parkstats : public command_t
{
 public:
 cmd_parkstats (InspIRCd* Instance) : command_t(Instance,"PARKSTATS", 'o', 0)
	{
		this->source = "m_park.so";
	}

	void Handle (const char** parameters, int pcnt, userrec *user)
	{
		char status[MAXBUF];
		snprintf(status,MAXBUF,"NOTICE %s :There are a total of %lu parked clients on this server, with a maximum of %lu parked sessions allowed per user.",user->nick,(unsigned long)pinfo.size(),(unsigned long)ConcurrentParks);
		user->WriteServ(std::string(status));
	}
};

class cmd_unpark : public command_t
{
 public:
 cmd_unpark (InspIRCd* Instance) : command_t(Instance,"UNPARK", 0, 2)
	{
		this->source = "m_park.so";
		syntax = "<nick> <key>";
	}

	void Handle (const char** parameters, int pcnt, userrec *user)
	{
		/** Unparking. complicated stuff.
		 *
		 * Unparking is done in several steps:
		 *
		 * (1) Check if the user is parked
		 * (2) Check the key of the user against the one provided
		 * (3) Part the user who issued the command from all of their channels so as not to confuse clients
		 * (4) Remove all the users UMODEs
		 * (5) Duplicate and switch file descriptors on the two users, and disconnect the dead one
		 * (6) Force a nickchange onto the user who issued the command forcing them to change to the parked nick
		 * (7) Force join the user into all the channels the parked nick is currently in (send them localized join and namelist)
		 * (8) Send the user the umodes of their new 'persona'
		 * (9) Spool any awaylog messages to the user
		 *
		 * And there you have it, easy huh (NOT)...
		 */
		userrec* unpark = ServerInstance->FindNick(parameters[0]);
		if (!unpark)
		{
			user->WriteServ("942 %s %s :Invalid user specified.",user->nick, parameters[0]);
			return;
		}
		awaylog* awy;
		unpark->GetExt("park_awaylog", awy);
		long* key;
		unpark->GetExt("park_key", key);
		if (!awy)
		{
			user->WriteServ("943 %s %s :This user is not parked.",user->nick, unpark->nick);
			return;
		}
		if (*key == atoi(parameters[1]))
		{
			// first part the user from all chans theyre on, so things dont get messy
			for (std::vector<ucrec*>::iterator i = user->chans.begin(); i != user->chans.end(); i++)
			{
				chanrec* chan = (*i)->channel;
				if (chan != NULL)
				{
					if (!chan->PartUser(user, "Unparking"))
						delete chan;
				}
			}
			// remove all their old modes
			user->WriteServ("MODE %s -%s",user->nick,user->FormatModes());
			// now, map them to the parked user, while nobody can see :p
			ServerInstance->PseudoToUser(user,unpark,"Unparked to "+std::string(parameters[0]));
			// set all their new modes
			unpark->WriteServ("MODE %s +%s",unpark->nick,unpark->FormatModes());
			// spool their away log to them
			unpark->WriteServ("NOTICE %s :*** You are now unparked. You have successfully taken back the nickname and privilages of %s.",unpark->nick,unpark->nick);
			for (awaylog::iterator i = awy->begin(); i != awy->end(); i++)
			{
				char timebuf[MAXBUF];
				tm *timeinfo = localtime(&i->tm);
				strlcpy(timebuf,asctime(timeinfo),MAXBUF);
				timebuf[strlen(timebuf)-1] = '\0';
				unpark->WriteServ("NOTICE %s :From %s at %s: \2%s\2",unpark->nick,i->from.c_str(),timebuf,i->text.c_str());
			}
			DELETE(awy);
			DELETE(key);
			unpark->Shrink("park_awaylog");
			unpark->Shrink("park_key");
			for (parkinfo::iterator j = pinfo.begin(); j != pinfo.end(); j++)
			{
				if (j->nick == std::string(unpark->nick))
				{
					pinfo.erase(j);
					break;
				}
			}
		}
		else
		{
			user->WriteServ("928 "+std::string(user->nick)+" :Incorrect park key.");
		}
	}
};

class ModulePark : public Module
{
 protected:
	ConfigReader* Conf;
	cmd_park*	cmd1;
	cmd_unpark*	cmd2;
	cmd_parkstats*	cmd3;
 public:
	virtual void ReadSettings()
	{
		Conf = new ConfigReader(ServerInstance);
		ParkMaxTime = Conf->ReadInteger("park","maxtime",0,true);
		ConcurrentParks = Conf->ReadInteger("park","maxperip",0,true);
		ParkMaxMsgs = Conf->ReadInteger("park","maxmessages",0,true);
		DELETE(Conf);
	}

	ModulePark(InspIRCd* Me)
		: Module::Module(Me)
	{
		
		pinfo.clear();
		this->ReadSettings();
		cmd1 = new cmd_park(ServerInstance);
		cmd2 = new cmd_unpark(ServerInstance);
		cmd3 = new cmd_parkstats(ServerInstance);
		ServerInstance->AddCommand(cmd1);
		ServerInstance->AddCommand(cmd2);
		ServerInstance->AddCommand(cmd3);
	}

	virtual ~ModulePark()
	{
	}

	void Implements(char* List)
	{
		List[I_On005Numeric] = List[I_OnRehash] = List[I_OnUserQuit] = List[I_OnUserPreMessage] = List[I_OnUserPreNick] = List[I_OnBackgroundTimer] = List[I_OnWhois] = 1;
	}

	virtual void OnRehash(const std::string &parameter)
	{
		this->ReadSettings();
	}

	virtual void On005Numeric(std::string &output)
	{
		output = output + std::string(" PARK");
	}

	virtual void OnUserQuit(userrec* user, const std::string &reason)
	{
		std::string nick = user->nick;
		// track quits in our parked user list
		for (parkinfo::iterator j = pinfo.begin(); j != pinfo.end(); j++)
		{
			if (j->nick == nick)
			{
				pinfo.erase(j);
				break;
			}
		}
	}


	virtual void OnPrePrivmsg(userrec* user, userrec* dest, const std::string &text)
	{
		awaylog* awy;
		dest->GetExt("park_awaylog", awy);
		if (awy)
		{
			if (awy->size() <= (unsigned)ParkMaxMsgs)
			{
				awaymsg am;
				am.text = text;
				am.from = user->GetFullHost();
				am.tm = time(NULL);
				awy->push_back(am);
				user->WriteServ("930 "+std::string(user->nick)+" :User "+std::string(dest->nick)+" is parked. Your message has been stored.");
			}
			else user->WriteServ("929 "+std::string(user->nick)+" :User "+std::string(dest->nick)+" is parked, but their message queue is full. Message not saved.");
		}
	}

	virtual int OnUserPreNick(userrec* user, const std::string &newnick)
	{
		// track nickchanges in our parked user list
		// (this isnt too efficient, i'll tidy it up some time)
		/* XXX - perhaps extend the user record, or, that wouldn't work - perhaps use a map? -- w00t */
		for (parkinfo::iterator j = pinfo.begin(); j != pinfo.end(); j++)
		{
			if (j->nick == std::string(user->nick))
			{
				j->nick = newnick;
				break;
			}
		}
		return 0;
	}

	virtual void OnBackgroundTimer(time_t curtime)
	{
		// look for parked clients which have timed out (this needs tidying)
		if (pinfo.empty())
			return;
		bool go_again = true;
		while (go_again)
		{
			go_again = false;
			for (parkinfo::iterator j = pinfo.begin(); j != pinfo.end(); j++)
			{
				if (time(NULL) >= (j->parktime+ParkMaxTime))
				{
					userrec* thisnick = ServerInstance->FindNick(j->nick);
					// THIS MUST COME BEFORE THE QuitUser - QuitUser can
					// create a recursive call to OnUserQuit in this module
					// and then corrupt the pointer!
					pinfo.erase(j);
					if (thisnick)
						userrec::QuitUser(ServerInstance,thisnick,"PARK timeout");
					go_again = true;
					break;
				}
			}
		}
	}

	/* XXX - why is OnPrePrivmsg seperated here, I assume there is reason for the extra function call? --w00t */
	virtual int OnUserPreMessage(userrec* user,void* dest,int target_type, std::string &text, char status)
	{
		if (target_type == TYPE_USER)
		{
			userrec* u = (userrec*)dest;
			OnPrePrivmsg(user,u,text);
		}
		return 0;
	}

	virtual void OnWhois(userrec* src, userrec* dst)
	{
		char* dummy;
		if (dst->GetExt("park_awaylog", dummy))
			src->WriteServ("335 "+std::string(src->nick)+" "+std::string(dst->nick)+" :is a parked client");
	}
	
	virtual Version GetVersion()
	{
		return Version(1,0,0,1,VF_VENDOR);
	}
	
};

// stuff down here is the module-factory stuff. For basic modules you can ignore this.

class ModuleParkFactory : public ModuleFactory
{
 public:
	ModuleParkFactory()
	{
	}
	
	~ModuleParkFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		return new ModulePark(Me);
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleParkFactory;
}

