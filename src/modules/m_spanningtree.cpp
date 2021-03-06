/*   +------------------------------------+
 *   | Inspire Internet Relay Chat Daemon |
 *   +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                     E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* $ModDesc: Povides a spanning tree server link protocol */

using namespace std;

#include <stdio.h>
#include <vector>
#include <deque>
#include "globals.h"
#include "inspircd_config.h"
#include "hash_map.h"
#include "configreader.h"
#include "users.h"
#include "channels.h"
#include "modules.h"
#include "commands.h"
#include "commands/cmd_whois.h"
#include "commands/cmd_stats.h"
#include "socket.h"

#include "inspircd.h"
#include "wildcard.h"
#include "inspstring.h"
#include "hashcomp.h"
#include "xline.h"
#include "typedefs.h"
#include "cull_list.h"
#include "aes.h"

#define nspace __gnu_cxx

/*
 * The server list in InspIRCd is maintained as two structures
 * which hold the data in different ways. Most of the time, we
 * want to very quicky obtain three pieces of information:
 *
 * (1) The information on a server
 * (2) The information on the server we must send data through
 *     to actually REACH the server we're after
 * (3) Potentially, the child/parent objects of this server
 *
 * The InspIRCd spanning protocol provides easy access to these
 * by storing the data firstly in a recursive structure, where
 * each item references its parent item, and a dynamic list
 * of child items, and another structure which stores the items
 * hashed, linearly. This means that if we want to find a server
 * by name quickly, we can look it up in the hash, avoiding
 * any O(n) lookups. If however, during a split or sync, we want
 * to apply an operation to a server, and any of its child objects
 * we can resort to recursion to walk the tree structure.
 */

using irc::sockets::MatchCIDR;

class ModuleSpanningTree;
static ModuleSpanningTree* TreeProtocolModule;
static InspIRCd* ServerInstance;

/* Any socket can have one of five states at any one time.
 * The LISTENER state indicates a socket which is listening
 * for connections. It cannot receive data itself, only incoming
 * sockets.
 * The CONNECTING state indicates an outbound socket which is
 * waiting to be writeable.
 * The WAIT_AUTH_1 state indicates the socket is outbound and
 * has successfully connected, but has not yet sent and received
 * SERVER strings.
 * The WAIT_AUTH_2 state indicates that the socket is inbound
 * (allocated by a LISTENER) but has not yet sent and received
 * SERVER strings.
 * The CONNECTED state represents a fully authorized, fully
 * connected server.
 */
enum ServerState { LISTENER, CONNECTING, WAIT_AUTH_1, WAIT_AUTH_2, CONNECTED };

/* Foward declarations */
class TreeServer;
class TreeSocket;

/* This variable represents the root of the server tree
 * (for all intents and purposes, it's us)
 */
TreeServer *TreeRoot;

/* This hash_map holds the hash equivalent of the server
 * tree, used for rapid linear lookups.
 */
typedef nspace::hash_map<std::string, TreeServer*, nspace::hash<string>, irc::StrHashComp> server_hash;
server_hash serverlist;

typedef nspace::hash_map<std::string, userrec*> uid_hash;
typedef nspace::hash_map<std::string, char*> sid_hash;

/* More forward declarations */
bool DoOneToOne(std::string prefix, std::string command, std::deque<std::string> &params, std::string target);
bool DoOneToAllButSender(std::string prefix, std::string command, std::deque<std::string> &params, std::string omit);
bool DoOneToMany(std::string prefix, std::string command, std::deque<std::string> &params);
bool DoOneToAllButSenderRaw(std::string data, std::string omit, std::string prefix, irc::string command, std::deque<std::string> &params);
void ReadConfiguration(bool rebind);

/* Flatten links and /MAP for non-opers */
bool FlatLinks;
/* Hide U-Lined servers in /MAP and /LINKS */
bool HideULines;

std::vector<std::string> ValidIPs;

class UserManager : public classbase
{
	uid_hash uids;
	sid_hash sids;
 public:
	UserManager()
	{
		uids.clear();
		sids.clear();
	}

	std::string UserToUID(userrec* user)
	{
		return "";
	}

	std::string UIDToUser(const std::string &UID)
	{
		return "";
	}

	std::string CreateAndAdd(userrec* user)
	{
		return "";
	}

	std::string CreateAndAdd(const std::string &servername)
	{
		return "";
	}

	std::string ServerToSID(const std::string &servername)
	{
		return "";
	}

	std::string SIDToServer(const std::string &SID)
	{
		return "";
	}

	userrec* FindByID(const std::string &UID)
	{
		return NULL;
	}
};


/* Each server in the tree is represented by one class of
 * type TreeServer. A locally connected TreeServer can
 * have a class of type TreeSocket associated with it, for
 * remote servers, the TreeSocket entry will be NULL.
 * Each server also maintains a pointer to its parent
 * (NULL if this server is ours, at the top of the tree)
 * and a pointer to its "Route" (see the comments in the
 * constructors below), and also a dynamic list of pointers
 * to its children which can be iterated recursively
 * if required. Creating or deleting objects of type
 i* TreeServer automatically maintains the hash_map of
 * TreeServer items, deleting and inserting them as they
 * are created and destroyed.
 */

class TreeServer : public classbase
{
	InspIRCd* ServerInstance;		/* Creator */
	TreeServer* Parent;			/* Parent entry */
	TreeServer* Route;			/* Route entry */
	std::vector<TreeServer*> Children;	/* List of child objects */
	irc::string ServerName;			/* Server's name */
	std::string ServerDesc;			/* Server's description */
	std::string VersionString;		/* Version string or empty string */
	int UserCount;				/* Not used in this version */
	int OperCount;				/* Not used in this version */
	TreeSocket* Socket;			/* For directly connected servers this points at the socket object */
	time_t NextPing;			/* After this time, the server should be PINGed*/
	bool LastPingWasGood;			/* True if the server responded to the last PING with a PONG */
	
 public:

	/* We don't use this constructor. Its a dummy, and won't cause any insertion
	 * of the TreeServer into the hash_map. See below for the two we DO use.
	 */
	TreeServer(InspIRCd* Instance) : ServerInstance(Instance)
	{
		Parent = NULL;
		ServerName = "";
		ServerDesc = "";
		VersionString = "";
		UserCount = OperCount = 0;
		VersionString = ServerInstance->GetVersionString();
	}

	/* We use this constructor only to create the 'root' item, TreeRoot, which
	 * represents our own server. Therefore, it has no route, no parent, and
	 * no socket associated with it. Its version string is our own local version.
	 */
	TreeServer(InspIRCd* Instance, std::string Name, std::string Desc) : ServerInstance(Instance), ServerName(Name.c_str()), ServerDesc(Desc)
	{
		Parent = NULL;
		VersionString = "";
		UserCount = OperCount = 0;
		VersionString = ServerInstance->GetVersionString();
		Route = NULL;
		Socket = NULL; /* Fix by brain */
		AddHashEntry();
	}

	/* When we create a new server, we call this constructor to initialize it.
	 * This constructor initializes the server's Route and Parent, and sets up
	 * its ping counters so that it will be pinged one minute from now.
	 */
	TreeServer(InspIRCd* Instance, std::string Name, std::string Desc, TreeServer* Above, TreeSocket* Sock)
		: ServerInstance(Instance), Parent(Above), ServerName(Name.c_str()), ServerDesc(Desc), Socket(Sock)
	{
		VersionString = "";
		UserCount = OperCount = 0;
		this->SetNextPingTime(time(NULL) + 120);
		this->SetPingFlag();

		/* find the 'route' for this server (e.g. the one directly connected
		 * to the local server, which we can use to reach it)
		 *
		 * In the following example, consider we have just added a TreeServer
		 * class for server G on our network, of which we are server A.
		 * To route traffic to G (marked with a *) we must send the data to
		 * B (marked with a +) so this algorithm initializes the 'Route'
		 * value to point at whichever server traffic must be routed through
		 * to get here. If we were to try this algorithm with server B,
		 * the Route pointer would point at its own object ('this').
		 *
		 *              A
		 *             / \
		 *          + B   C
		 *           / \   \
		 *          D   E   F
		 *         /         \
		 *      * G           H
		 *
		 * We only run this algorithm when a server is created, as
		 * the routes remain constant while ever the server exists, and
		 * do not need to be re-calculated.
		 */

		Route = Above;
		if (Route == TreeRoot)
		{
			Route = this;
		}
		else
		{
			while (this->Route->GetParent() != TreeRoot)
			{
				this->Route = Route->GetParent();
			}
		}

		/* Because recursive code is slow and takes a lot of resources,
		 * we store two representations of the server tree. The first
		 * is a recursive structure where each server references its
		 * children and its parent, which is used for netbursts and
		 * netsplits to dump the whole dataset to the other server,
		 * and the second is used for very fast lookups when routing
		 * messages and is instead a hash_map, where each item can
		 * be referenced by its server name. The AddHashEntry()
		 * call below automatically inserts each TreeServer class
		 * into the hash_map as it is created. There is a similar
		 * maintainance call in the destructor to tidy up deleted
		 * servers.
		 */

		this->AddHashEntry();
	}

	int QuitUsers(const std::string &reason)
	{
		ServerInstance->Log(DEBUG,"Removing all users from server %s",this->ServerName.c_str());
		const char* reason_s = reason.c_str();
		std::vector<userrec*> time_to_die;
		for (user_hash::iterator n = ServerInstance->clientlist.begin(); n != ServerInstance->clientlist.end(); n++)
		{
			if (!strcmp(n->second->server, this->ServerName.c_str()))
			{
				time_to_die.push_back(n->second);
			}
		}
		for (std::vector<userrec*>::iterator n = time_to_die.begin(); n != time_to_die.end(); n++)
		{
			userrec* a = (userrec*)*n;
			ServerInstance->Log(DEBUG,"Kill %s fd=%d",a->nick,a->GetFd());
			if (!IS_LOCAL(a))
				userrec::QuitUser(ServerInstance,a,reason_s);
		}
		return time_to_die.size();
	}

	/* This method is used to add the structure to the
	 * hash_map for linear searches. It is only called
	 * by the constructors.
	 */
	void AddHashEntry()
	{
		server_hash::iterator iter;
		iter = serverlist.find(this->ServerName.c_str());
		if (iter == serverlist.end())
			serverlist[this->ServerName.c_str()] = this;
	}

	/* This method removes the reference to this object
	 * from the hash_map which is used for linear searches.
	 * It is only called by the default destructor.
	 */
	void DelHashEntry()
	{
		server_hash::iterator iter;
		iter = serverlist.find(this->ServerName.c_str());
		if (iter != serverlist.end())
			serverlist.erase(iter);
	}

	/* These accessors etc should be pretty self-
	 * explanitory.
	 */

	TreeServer* GetRoute()
	{
		return Route;
	}

	std::string GetName()
	{
		return ServerName.c_str();
	}

	std::string GetDesc()
	{
		return ServerDesc;
	}

	std::string GetVersion()
	{
		return VersionString;
	}

	void SetNextPingTime(time_t t)
	{
		this->NextPing = t;
		LastPingWasGood = false;
	}

	time_t NextPingTime()
	{
		return NextPing;
	}

	bool AnsweredLastPing()
	{
		return LastPingWasGood;
	}

	void SetPingFlag()
	{
		LastPingWasGood = true;
	}

	int GetUserCount()
	{
		return UserCount;
	}

	void AddUserCount()
	{
		UserCount++;
	}

	void DelUserCount()
	{
		UserCount--;
	}

	int GetOperCount()
	{
		return OperCount;
	}

	TreeSocket* GetSocket()
	{
		return Socket;
	}

	TreeServer* GetParent()
	{
		return Parent;
	}

	void SetVersion(std::string Version)
	{
		VersionString = Version;
	}

	unsigned int ChildCount()
	{
		return Children.size();
	}

	TreeServer* GetChild(unsigned int n)
	{
		if (n < Children.size())
		{
			/* Make sure they  cant request
			 * an out-of-range object. After
			 * all we know what these programmer
			 * types are like *grin*.
			 */
			return Children[n];
		}
		else
		{
			return NULL;
		}
	}

	void AddChild(TreeServer* Child)
	{
		Children.push_back(Child);
	}

	bool DelChild(TreeServer* Child)
	{
		for (std::vector<TreeServer*>::iterator a = Children.begin(); a < Children.end(); a++)
		{
			if (*a == Child)
			{
				Children.erase(a);
				return true;
			}
		}
		return false;
	}

	/* Removes child nodes of this node, and of that node, etc etc.
	 * This is used during netsplits to automatically tidy up the
	 * server tree. It is slow, we don't use it for much else.
	 */
	bool Tidy()
	{
		bool stillchildren = true;
		while (stillchildren)
		{
			stillchildren = false;
			for (std::vector<TreeServer*>::iterator a = Children.begin(); a < Children.end(); a++)
			{
				TreeServer* s = (TreeServer*)*a;
				s->Tidy();
				Children.erase(a);
				DELETE(s);
				stillchildren = true;
				break;
			}
		}
		return true;
	}

	~TreeServer()
	{
		/* We'd better tidy up after ourselves, eh? */
		this->DelHashEntry();
	}
};

/* The Link class might as well be a struct,
 * but this is C++ and we don't believe in structs (!).
 * It holds the entire information of one <link>
 * tag from the main config file. We maintain a list
 * of them, and populate the list on rehash/load.
 */

class Link : public classbase
{
 public:
	 irc::string Name;
	 std::string IPAddr;
	 int Port;
	 std::string SendPass;
	 std::string RecvPass;
	 unsigned long AutoConnect;
	 time_t NextConnectTime;
	 std::string EncryptionKey;
	 bool HiddenFromStats;
};

/* The usual stuff for inspircd modules,
 * plus the vector of Link classes which we
 * use to store the <link> tags from the config
 * file.
 */
ConfigReader *Conf;
std::vector<Link> LinkBlocks;

/* Yay for fast searches!
 * This is hundreds of times faster than recursion
 * or even scanning a linked list, especially when
 * there are more than a few servers to deal with.
 * (read as: lots).
 */
TreeServer* FindServer(std::string ServerName)
{
	server_hash::iterator iter;
	iter = serverlist.find(ServerName.c_str());
	if (iter != serverlist.end())
	{
		return iter->second;
	}
	else
	{
		return NULL;
	}
}

/* Returns the locally connected server we must route a
 * message through to reach server 'ServerName'. This
 * only applies to one-to-one and not one-to-many routing.
 * See the comments for the constructor of TreeServer
 * for more details.
 */
TreeServer* BestRouteTo(std::string ServerName)
{
	if (ServerName.c_str() == TreeRoot->GetName())
		return NULL;
	TreeServer* Found = FindServer(ServerName);
	if (Found)
	{
		return Found->GetRoute();
	}
	else
	{
		return NULL;
	}
}

/* Find the first server matching a given glob mask.
 * Theres no find-using-glob method of hash_map [awwww :-(]
 * so instead, we iterate over the list using an iterator
 * and match each one until we get a hit. Yes its slow,
 * deal with it.
 */
TreeServer* FindServerMask(std::string ServerName)
{
	for (server_hash::iterator i = serverlist.begin(); i != serverlist.end(); i++)
	{
		if (match(i->first.c_str(),ServerName.c_str()))
			return i->second;
	}
	return NULL;
}

/* A convenient wrapper that returns true if a server exists */
bool IsServer(std::string ServerName)
{
	return (FindServer(ServerName) != NULL);
}


class cmd_rconnect : public command_t
{
	Module* Creator;
 public:
	cmd_rconnect (InspIRCd* Instance, Module* Callback) : command_t(Instance, "RCONNECT", 'o', 2), Creator(Callback)
	{
		this->source = "m_spanningtree.so";
		syntax = "<remote-server-mask> <servermask>";
	}

	void Handle (const char** parameters, int pcnt, userrec *user)
	{
		user->WriteServ("NOTICE %s :*** RCONNECT: Sending remote connect to \002%s\002 to connect server \002%s\002.",user->nick,parameters[0],parameters[1]);
		/* Is this aimed at our server? */
		if (ServerInstance->MatchText(ServerInstance->Config->ServerName,parameters[0]))
		{
			/* Yes, initiate the given connect */
			ServerInstance->WriteOpers("*** Remote CONNECT from %s matching \002%s\002, connecting server \002%s\002",user->nick,parameters[0],parameters[1]);
			const char* para[1];
			para[0] = parameters[1];
			Creator->OnPreCommand("CONNECT", para, 1, user, true);
		}
	}
};
 


/* Every SERVER connection inbound or outbound is represented by
 * an object of type TreeSocket.
 * TreeSockets, being inherited from InspSocket, can be tied into
 * the core socket engine, and we cn therefore receive activity events
 * for them, just like activex objects on speed. (yes really, that
 * is a technical term!) Each of these which relates to a locally
 * connected server is assocated with it, by hooking it onto a
 * TreeSocket class using its constructor. In this way, we can
 * maintain a list of servers, some of which are directly connected,
 * some of which are not.
 */

class TreeSocket : public InspSocket
{
	std::string myhost;
	std::string in_buffer;
	ServerState LinkState;
	std::string InboundServerName;
	std::string InboundDescription;
	int num_lost_users;
	int num_lost_servers;
	time_t NextPing;
	bool LastPingWasGood;
	bool bursting;
	AES* ctx_in;
	AES* ctx_out;
	unsigned int keylength;
	
 public:

	/* Because most of the I/O gubbins are encapsulated within
	 * InspSocket, we just call the superclass constructor for
	 * most of the action, and append a few of our own values
	 * to it.
	 */
	TreeSocket(InspIRCd* SI, std::string host, int port, bool listening, unsigned long maxtime)
		: InspSocket(SI, host, port, listening, maxtime)
	{
		myhost = host;
		this->LinkState = LISTENER;
		this->ctx_in = NULL;
		this->ctx_out = NULL;
	}

	TreeSocket(InspIRCd* SI, std::string host, int port, bool listening, unsigned long maxtime, std::string ServerName)
		: InspSocket(SI, host, port, listening, maxtime)
	{
		myhost = ServerName;
		this->LinkState = CONNECTING;
		this->ctx_in = NULL;
		this->ctx_out = NULL;
	}

	/* When a listening socket gives us a new file descriptor,
	 * we must associate it with a socket without creating a new
	 * connection. This constructor is used for this purpose.
	 */
	TreeSocket(InspIRCd* SI, int newfd, char* ip)
		: InspSocket(SI, newfd, ip)
	{
		this->LinkState = WAIT_AUTH_1;
		this->ctx_in = NULL;
		this->ctx_out = NULL;
		this->SendCapabilities();
	}

	~TreeSocket()
	{
		if (ctx_in)
			DELETE(ctx_in);
		if (ctx_out)
			DELETE(ctx_out);
	}

	void InitAES(std::string key,std::string SName)
	{
		if (key == "")
			return;

		ctx_in = new AES();
		ctx_out = new AES();
		ServerInstance->Log(DEBUG,"Initialized AES key %s",key.c_str());
		// key must be 16, 24, 32 etc bytes (multiple of 8)
		keylength = key.length();
		if (!(keylength == 16 || keylength == 24 || keylength == 32))
		{
			this->Instance->WriteOpers("*** \2ERROR\2: Key length for encryptionkey is not 16, 24 or 32 bytes in length!");
			ServerInstance->Log(DEBUG,"Key length not 16, 24 or 32 characters!");
		}
		else
		{
			this->Instance->WriteOpers("*** \2AES\2: Initialized %d bit encryption to server %s",keylength*8,SName.c_str());
			ctx_in->MakeKey(key.c_str(), "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
				\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", keylength, keylength);
			ctx_out->MakeKey(key.c_str(), "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\
				\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", keylength, keylength);
		}
	}
	
	/* When an outbound connection finishes connecting, we receive
	 * this event, and must send our SERVER string to the other
	 * side. If the other side is happy, as outlined in the server
	 * to server docs on the inspircd.org site, the other side
	 * will then send back its own server string.
	 */
	virtual bool OnConnected()
	{
		if (this->LinkState == CONNECTING)
		{
			/* we do not need to change state here. */
			for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
			{
				if (x->Name == this->myhost)
				{
					this->Instance->WriteOpers("*** Connection to \2"+myhost+"\2["+(x->HiddenFromStats ? "<hidden>" : this->GetIP())+"] established.");
					this->SendCapabilities();
					if (x->EncryptionKey != "")
					{
						if (!(x->EncryptionKey.length() == 16 || x->EncryptionKey.length() == 24 || x->EncryptionKey.length() == 32))
						{
							this->Instance->WriteOpers("\2WARNING\2: Your encryption key is NOT 16, 24 or 32 characters in length, encryption will \2NOT\2 be enabled.");
						}
						else
						{
							this->WriteLine(std::string("AES ")+this->Instance->Config->ServerName);
							this->InitAES(x->EncryptionKey,x->Name.c_str());
						}
					}
					/* found who we're supposed to be connecting to, send the neccessary gubbins. */
					this->WriteLine(std::string("SERVER ")+this->Instance->Config->ServerName+" "+x->SendPass+" 0 :"+this->Instance->Config->ServerDesc);
					return true;
				}
			}
		}
		/* There is a (remote) chance that between the /CONNECT and the connection
		 * being accepted, some muppet has removed the <link> block and rehashed.
		 * If that happens the connection hangs here until it's closed. Unlikely
		 * and rather harmless.
		 */
		this->Instance->WriteOpers("*** Connection to \2"+myhost+"\2 lost link tag(!)");
		return true;
	}
	
	virtual void OnError(InspSocketError e)
	{
		/* We don't handle this method, because all our
		 * dirty work is done in OnClose() (see below)
		 * which is still called on error conditions too.
		 */
		if (e == I_ERR_CONNECT)
		{
			this->Instance->WriteOpers("*** Connection failed: Connection refused");
		}
	}

	virtual int OnDisconnect()
	{
		/* For the same reason as above, we don't
		 * handle OnDisconnect()
		 */
		return true;
	}

	/* Recursively send the server tree with distances as hops.
	 * This is used during network burst to inform the other server
	 * (and any of ITS servers too) of what servers we know about.
	 * If at any point any of these servers already exist on the other
	 * end, our connection may be terminated. The hopcounts given
	 * by this function are relative, this doesn't matter so long as
	 * they are all >1, as all the remote servers re-calculate them
	 * to be relative too, with themselves as hop 0.
	 */
	void SendServers(TreeServer* Current, TreeServer* s, int hops)
	{
		char command[1024];
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			TreeServer* recursive_server = Current->GetChild(q);
			if (recursive_server != s)
			{
				snprintf(command,1024,":%s SERVER %s * %d :%s",Current->GetName().c_str(),recursive_server->GetName().c_str(),hops,recursive_server->GetDesc().c_str());
				this->WriteLine(command);
				this->WriteLine(":"+recursive_server->GetName()+" VERSION :"+recursive_server->GetVersion());
				/* down to next level */
				this->SendServers(recursive_server, s, hops+1);
			}
		}
	}

	std::string MyCapabilities()
	{
		std::vector<std::string> modlist;
		std::string capabilities = "";

		for (int i = 0; i <= this->Instance->GetModuleCount(); i++)
		{
			if ((this->Instance->modules[i]->GetVersion().Flags & VF_STATIC) || (this->Instance->modules[i]->GetVersion().Flags & VF_COMMON))
				modlist.push_back(this->Instance->Config->module_names[i]);
		}
		sort(modlist.begin(),modlist.end());
		for (unsigned int i = 0; i < modlist.size(); i++)
		{
			if (i)
				capabilities = capabilities + ",";
			capabilities = capabilities + modlist[i];
		}
		return capabilities;
	}
	
	void SendCapabilities()
	{
		this->WriteLine("CAPAB "+MyCapabilities());
	}

	bool Capab(std::deque<std::string> params)
	{
		if (params.size() != 1)
		{
			this->WriteLine("ERROR :Invalid number of parameters for CAPAB");
			return false;
		}

		if (params[0] != this->MyCapabilities())
		{
			std::string quitserver = this->myhost;
			if (this->InboundServerName != "")
			{
				quitserver = this->InboundServerName;
			}

			this->Instance->WriteOpers("*** \2ERROR\2: Server '%s' does not have the same set of modules loaded, cannot link!",quitserver.c_str());
			this->Instance->WriteOpers("*** Our networked module set is: '%s'",this->MyCapabilities().c_str());
			this->Instance->WriteOpers("*** Other server's networked module set is: '%s'",params[0].c_str());
			this->Instance->WriteOpers("*** These lists must match exactly on both servers. Please correct these errors, and try again.");
			this->WriteLine("ERROR :CAPAB mismatch; My capabilities: '"+this->MyCapabilities()+"'");
			return false;
		}

		return true;
	}

	/* This function forces this server to quit, removing this server
	 * and any users on it (and servers and users below that, etc etc).
	 * It's very slow and pretty clunky, but luckily unless your network
	 * is having a REAL bad hair day, this function shouldnt be called
	 * too many times a month ;-)
	 */
	void SquitServer(std::string &from, TreeServer* Current)
	{
		/* recursively squit the servers attached to 'Current'.
		 * We're going backwards so we don't remove users
		 * while we still need them ;)
		 */
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			TreeServer* recursive_server = Current->GetChild(q);
			this->SquitServer(from,recursive_server);
		}
		/* Now we've whacked the kids, whack self */
		num_lost_servers++;
		num_lost_users += Current->QuitUsers(from);
	}

	/* This is a wrapper function for SquitServer above, which
	 * does some validation first and passes on the SQUIT to all
	 * other remaining servers.
	 */
	void Squit(TreeServer* Current,std::string reason)
	{
		if ((Current) && (Current != TreeRoot))
		{
			std::deque<std::string> params;
			params.push_back(Current->GetName());
			params.push_back(":"+reason);
			DoOneToAllButSender(Current->GetParent()->GetName(),"SQUIT",params,Current->GetName());
			if (Current->GetParent() == TreeRoot)
			{
				this->Instance->WriteOpers("Server \002"+Current->GetName()+"\002 split: "+reason);
			}
			else
			{
				this->Instance->WriteOpers("Server \002"+Current->GetName()+"\002 split from server \002"+Current->GetParent()->GetName()+"\002 with reason: "+reason);
			}
			num_lost_servers = 0;
			num_lost_users = 0;
			std::string from = Current->GetParent()->GetName()+" "+Current->GetName();
			SquitServer(from, Current);
			Current->Tidy();
			Current->GetParent()->DelChild(Current);
			DELETE(Current);
			this->Instance->WriteOpers("Netsplit complete, lost \002%d\002 users on \002%d\002 servers.", num_lost_users, num_lost_servers);
		}
		else
		{
			ServerInstance->Log(DEFAULT,"Squit from unknown server");
		}
	}

	/* FMODE command - server mode with timestamp checks */
	bool ForceMode(std::string source, std::deque<std::string> &params)
	{
		/* Chances are this is a 1.0 FMODE without TS */
		if (params.size() < 3)
		{
			this->WriteLine("ERROR :Version 1.0 FMODE sent to version 1.1 server");
			return false;
		}
		
		bool smode = false;
		std::string sourceserv;

		/* Are we dealing with an FMODE from a user, or from a server? */
		userrec* who = this->Instance->FindNick(source);
		if (who)
		{
			/* FMODE from a user, set sourceserv to the users server name */
			sourceserv = who->server;
		}
		else
		{
			/* FMODE from a server, create a fake user to receive mode feedback */
			who = new userrec(this->Instance);
			who->SetFd(FD_MAGIC_NUMBER);
			smode = true;		/* Setting this flag tells us we should free the userrec later */
			sourceserv = source;	/* Set sourceserv to the actual source string */
		}
		const char* modelist[64];
		time_t TS = 0;
		int n = 0;
		memset(&modelist,0,sizeof(modelist));
		for (unsigned int q = 0; (q < params.size()) && (q < 64); q++)
		{
			if (q == 1)
			{
				/* The timestamp is in this position.
				 * We don't want to pass that up to the
				 * server->client protocol!
				 */
				TS = atoi(params[q].c_str());
			}
			else
			{
				/* Everything else is fine to append to the modelist */
				modelist[n++] = params[q].c_str();
			}
				
		}
                /* Extract the TS value of the object, either userrec or chanrec */
		userrec* dst = this->Instance->FindNick(params[0]);
		chanrec* chan = NULL;
		time_t ourTS = 0;
		if (dst)
		{
			ourTS = dst->age;
		}
		else
		{
			chan = this->Instance->FindChan(params[0]);
			if (chan)
			{
				ourTS = chan->age;
			}
		}

		/* TS is equal: Merge the mode changes, use voooodoooooo on modes
		 * with parameters.
		 */
		if (TS == ourTS)
		{
			ServerInstance->Log(DEBUG,"Entering TS equality check");
			ModeHandler* mh = NULL;
			unsigned long paramptr = 3;
			std::string to_bounce = "";
			std::string to_keep = "";
			std::vector<std::string> params_to_keep;
			std::string params_to_bounce = "";
			bool adding = true;
			char cur_change = 1;
			char old_change = 0;
			char old_bounce_change = 0;
			/* Merge modes, basically do special stuff to mode with params */
			for (std::string::iterator x = params[2].begin(); x != params[2].end(); x++)
			{
				switch (*x)
				{
					case '-':
						adding = false;
					break;
					case '+':
						adding = true;
					break;
					default:
						if (adding)
						{
							/* We only care about whats being set,
							 * not whats being unset
							 */
							mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

							if ((mh) && (mh->GetNumParams(adding) > 0) && (!mh->IsListMode()))
							{
								/* We only want to do special things to
								 * modes with parameters, we are going to rewrite
								 * those parameters
								 */
								ModePair ret;
								adding ? cur_change = '+' : cur_change = '-';

								ret = mh->ModeSet(smode ? NULL : who, dst, chan, params[paramptr]);

								/* The mode is set here, check which we should keep */
								if (ret.first)
								{
									bool which_to_keep = mh->CheckTimeStamp(TS, ourTS, params[paramptr], ret.second, chan);

									if (which_to_keep == true)
									{
										/* Keep ours, bounce theirs:
										 * Send back ours to them and
										 * drop their mode changs
										 */
										adding ? cur_change = '+' : cur_change = '-';
										if (cur_change != old_bounce_change)
											to_bounce += cur_change;
										to_bounce += *x;
										old_bounce_change = cur_change;

										if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
											params_to_bounce.append(" ").append(ret.second);
									}
									else
									{
										/* Keep theirs: Accept their mode change,
										 * do nothing else
										 */
										adding ? cur_change = '+' : cur_change = '-';
										if (cur_change != old_change)
											to_keep += cur_change;
										to_keep += *x;
										old_change = cur_change;

										if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
											params_to_keep.push_back(params[paramptr]);
									}
								}
								else
								{
									/* Mode isnt set here, we want it */
									adding ? cur_change = '+' : cur_change = '-';
									if (cur_change != old_change)
										to_keep += cur_change;
									to_keep += *x;
									old_change = cur_change;

									if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
										params_to_keep.push_back(params[paramptr]);
								}

								paramptr++;
							}
							else
							{
								mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

								if (mh)
								{
									adding ? cur_change = '+' : cur_change = '-';
	
									/* Just keep this, safe to merge with no checks
									 * it has no parameters
									 */
	
									if (cur_change != old_change)
										to_keep += cur_change;
									to_keep += *x;
									old_change = cur_change;
	
									if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
									{
										ServerInstance->Log(DEBUG,"Mode removal %d %d",adding, mh->GetNumParams(adding));
										params_to_keep.push_back(params[paramptr++]);
									}
								}
							}
						}
						else
						{
							mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

							if (mh)
							{
								/* Taking a mode away */
								adding ? cur_change = '+' : cur_change = '-';

								if (cur_change != old_change)
									to_keep += cur_change;
								to_keep += *x;
								old_change = cur_change;

								if ((mh->GetNumParams(adding) > 0) && (paramptr < params.size()))
									params_to_keep.push_back(params[paramptr++]);
							}
						}
					break;
				}
			}

			if (to_bounce.length())
			{
				std::deque<std::string> newparams;
				newparams.push_back(params[0]);
				newparams.push_back(ConvToStr(ourTS));
				newparams.push_back(to_bounce+params_to_bounce);
				DoOneToOne(this->Instance->Config->ServerName,"FMODE",newparams,sourceserv);
			}

			if (to_keep.length())
			{
				unsigned int n = 2;
				unsigned int q = 0;
				modelist[0] = params[0].c_str();
				modelist[1] = to_keep.c_str();

				if (params_to_keep.size() > 2)
				{
					for (q = 2; (q < params_to_keep.size()) && (q < 64); q++)
					{
						ServerInstance->Log(DEBUG,"Item %d of %d", q, params_to_keep.size());
						modelist[n++] = params_to_keep[q].c_str();
					}
				}

                	        if (smode)
				{
					ServerInstance->Log(DEBUG,"Send mode");
					this->Instance->SendMode(modelist, n+2, who);
				}
				else
				{
					ServerInstance->Log(DEBUG,"Send mode client");
					this->Instance->CallCommandHandler("MODE", modelist, n+2, who);
				}

				/* HOT POTATO! PASS IT ON! */
				DoOneToAllButSender(source,"FMODE",params,sourceserv);
			}
		}
		else
		/* U-lined servers always win regardless of their TS */
		if ((TS > ourTS) && (!this->Instance->ULine(source.c_str())))
		{
			/* Bounce the mode back to its sender.* We use our lower TS, so the other end
			 * SHOULD accept it, if its clock is right.
			 *
			 * NOTE: We should check that we arent bouncing anything thats already set at this end.
			 * If we are, bounce +ourmode to 'reinforce' it. This prevents desyncs.
			 * e.g. They send +l 50, we have +l 10 set. rather than bounce -l 50, we bounce +l 10.
			 *
			 * Thanks to jilles for pointing out this one-hell-of-an-issue before i even finished
			 * writing the code. It took me a while to come up with this solution.
			 *
			 * XXX: BE SURE YOU UNDERSTAND THIS CODE FULLY BEFORE YOU MESS WITH IT.
			 */

			std::deque<std::string> newparams;	/* New parameter list we send back */
			newparams.push_back(params[0]);		/* Target, user or channel */
			newparams.push_back(ConvToStr(ourTS));	/* Timestamp value of the target */
			newparams.push_back("");		/* This contains the mode string. For now
								 * it's empty, we fill it below.
								 */

			/* Intelligent mode bouncing. Don't just invert, reinforce any modes which are already
			 * set to avoid a desync here.
			 */
			std::string modebounce = "";
			bool adding = true;
			unsigned int t = 3;
			ModeHandler* mh = NULL;
			char cur_change = 1;
			char old_change = 0;
			for (std::string::iterator x = params[2].begin(); x != params[2].end(); x++)
			{
				/* Iterate over all mode chars in the sent set */
				switch (*x)
				{
					/* Adding or subtracting modes? */
					case '-':
						adding = false;
					break;
					case '+':
						adding = true;
					break;
					default:
						/* Find the mode handler for this mode */
						mh = this->Instance->Modes->FindMode(*x, chan ? MODETYPE_CHANNEL : MODETYPE_USER);

						/* Got a mode handler?
						 * This also prevents us bouncing modes we have no handler for.
						 */
						if (mh)
						{
							ModePair ret;
							std::string p = "";

							/* Does the mode require a parameter right now?
							 * If it does, fetch it if we can
							 */
							if ((mh->GetNumParams(adding) > 0) && (t < params.size()))
								p = params[t++];

							/* Call the ModeSet method to determine if its set with the
							 * given parameter here or not.
							 */
							ret = mh->ModeSet(smode ? NULL : who, dst, chan, p);

							/* XXX: Really. Dont ask.
							 * Determine from if its set combined with what the current
							 * 'state' is (adding or not) as to wether we should 'invert'
							 * or 'reinforce' the mode change
							 */
							(!ret.first ? (adding ? cur_change = '-' : cur_change = '+') : (!adding ? cur_change = '-' : cur_change = '+'));

							/* Quickly determine if we have 'flipped' from + to -,
							 * or - to +, to prevent unneccessary +/- chars in the
							 * output string that waste bandwidth
							 */
							if (cur_change != old_change)
								modebounce += cur_change;
							old_change = cur_change;

							/* Add the mode character to the output string */
							modebounce += mh->GetModeChar();

							/* We got a parameter back from ModeHandler::ModeSet,
							 * are we supposed to be sending one out right now?
							 */
							if (ret.second.length())
							{
								if (mh->GetNumParams(cur_change == '+') > 0)
									/* Yes we're supposed to be sending out
									 * the parameter. Make sure it goes
									 */
									newparams.push_back(ret.second);
							}

						}
					break;
				}
			}
			
			/* Update the parameters for FMODE with the new 'bounced' string */
			newparams[2] = modebounce;
			/* Only send it back the way it came, no need to send it anywhere else */
			DoOneToOne(this->Instance->Config->ServerName,"FMODE",newparams,sourceserv);
			ServerInstance->Log(DEBUG,"FMODE bounced intelligently, our TS less than theirs and the other server is NOT a uline.");
		}
		else
		{
			ServerInstance->Log(DEBUG,"Allow modes, TS lower for sender");
			/* The server was ulined, but something iffy is up with the TS.
			 * Sound the alarm bells!
			 */
			if ((this->Instance->ULine(sourceserv.c_str())) && (TS > ourTS))
			{
				this->Instance->WriteOpers("\2WARNING!\2 U-Lined server '%s' has bad TS for '%s' (accepted change): \2SYNC YOUR CLOCKS\2 to avoid this notice",sourceserv.c_str(),params[0].c_str());
			}
			/* Allow the mode, route it to either server or user command handling */
			if (smode)
				this->Instance->SendMode(modelist,n,who);
			else
				this->Instance->CallCommandHandler("MODE", modelist, n, who);

			/* HOT POTATO! PASS IT ON! */
			DoOneToAllButSender(source,"FMODE",params,sourceserv);
		}
		/* Are we supposed to free the userrec? */
		if (smode)
			DELETE(who);

		return true;
	}

	/* FTOPIC command */
	bool ForceTopic(std::string source, std::deque<std::string> &params)
	{
		if (params.size() != 4)
			return true;
		time_t ts = atoi(params[1].c_str());
		std::string nsource = source;

		chanrec* c = this->Instance->FindChan(params[0]);
		if (c)
		{
			if ((ts >= c->topicset) || (!*c->topic))
			{
				std::string oldtopic = c->topic;
				strlcpy(c->topic,params[3].c_str(),MAXTOPIC);
				strlcpy(c->setby,params[2].c_str(),NICKMAX-1);
				c->topicset = ts;
				/* if the topic text is the same as the current topic,
				 * dont bother to send the TOPIC command out, just silently
				 * update the set time and set nick.
				 */
				if (oldtopic != params[3])
				{
					userrec* user = this->Instance->FindNick(source);
					if (!user)
					{
						c->WriteChannelWithServ(source.c_str(), "TOPIC %s :%s", c->name, c->topic);
					}
					else
					{
						c->WriteChannel(user, "TOPIC %s :%s", c->name, c->topic);
						nsource = user->server;
					}
					/* all done, send it on its way */
					params[3] = ":" + params[3];
					DoOneToAllButSender(source,"FTOPIC",params,nsource);
				}
			}
			
		}

		return true;
	}

	/* FJOIN, similar to unreal SJOIN */
	bool ForceJoin(std::string source, std::deque<std::string> &params)
	{
		if (params.size() < 3)
			return true;

		char first[MAXBUF];
		char modestring[MAXBUF];
		char* mode_users[127];
		memset(&mode_users,0,sizeof(mode_users));
		mode_users[0] = first;
		mode_users[1] = modestring;
		strcpy(modestring,"+");
		unsigned int modectr = 2;
		
		userrec* who = NULL;
		std::string channel = params[0];
		time_t TS = atoi(params[1].c_str());
		char* key = "";
		
		chanrec* chan = this->Instance->FindChan(channel);
		if (chan)
		{
			key = chan->key;
		}
		strlcpy(mode_users[0],channel.c_str(),MAXBUF);

		/* default is a high value, which if we dont have this
		 * channel will let the other side apply their modes.
		 */
		time_t ourTS = time(NULL)+600;
		chanrec* us = this->Instance->FindChan(channel);
		if (us)
		{
			ourTS = us->age;
		}

		ServerInstance->Log(DEBUG,"FJOIN detected, our TS=%lu, their TS=%lu",ourTS,TS);

		irc::tokenstream users(params[2]);
		std::string item = "";

		/* do this first, so our mode reversals are correctly received by other servers
		 * if there is a TS collision.
		 */
		params[2] = ":" + params[2];
		DoOneToAllButSender(source,"FJOIN",params,source);
		
		while ((item = users.GetToken()) != "")
		{
			/* process one channel at a time, applying modes. */
			char* usr = (char*)item.c_str();
			/* Safety check just to make sure someones not sent us an FJOIN full of spaces
			 * (is this even possible?) */
			if (usr && *usr)
			{
				char* permissions = usr;
				while ((*permissions) && (*permissions != ','))
				{
					ModeHandler* mh = ServerInstance->Modes->FindPrefix(*permissions);
					if (mh)
					{
						mode_users[modectr++] = usr;
						charlcat(modestring,mh->GetModeChar(),MAXBUF);
					}
					else
					{
						this->Instance->WriteOpers("ERROR: We received a user with an unknown prefix '%c'. Closed connection to avoid a desync.",mh->GetPrefix());
						this->WriteLine(std::string("ERROR :What?! You sent me a mode prefix i cant handle ('")+mh->GetModeChar()+"'). Closing connection to avoid desync.");
						return false;
					}
					usr++;
					permissions++;
				}
				who = this->Instance->FindNick(usr);
				if (who)
				{
					chanrec::JoinUser(this->Instance, who, channel.c_str(), true, key);
					if (modectr >= (MAXMODES-1))
					{
						/* theres a mode for this user. push them onto the mode queue, and flush it
						 * if there are more than MAXMODES to go.
						 */
						if ((ourTS >= TS) || (this->Instance->ULine(who->server)))
						{
							/* We also always let u-lined clients win, no matter what the TS value */
							ServerInstance->Log(DEBUG,"Our our channel newer than theirs, accepting their modes");
							this->Instance->SendMode((const char**)mode_users,modectr,who);
							if (ourTS != TS)
							{
								ServerInstance->Log(DEFAULT,"Channel TS for %s changed from %lu to %lu",us->name,ourTS,TS);
								us->age = TS;
								ourTS = TS;
							}
						}
						else
						{
							ServerInstance->Log(DEBUG,"Their channel newer than ours, bouncing their modes");
							/* bouncy bouncy! */
							std::deque<std::string> params;
							/* modes are now being UNSET... */
							*mode_users[1] = '-';
							for (unsigned int x = 0; x < modectr; x++)
							{
								if (x == 1)
								{
									params.push_back(ConvToStr(us->age));
								}
								params.push_back(mode_users[x]);
								
							}
							// tell everyone to bounce the modes. bad modes, bad!
							DoOneToMany(this->Instance->Config->ServerName,"FMODE",params);
						}
						strcpy(mode_users[1],"+");
						modectr = 2;
					}
				}
			}
		}
		/* there werent enough modes built up to flush it during FJOIN,
		 * or, there are a number left over. flush them out.
		 */
		if ((modectr > 2) && (who) && (us))
		{
			if (ourTS >= TS)
			{
				ServerInstance->Log(DEBUG,"Our our channel newer than theirs, accepting their modes");
				this->Instance->SendMode((const char**)mode_users,modectr,who);
				if (ourTS != TS)
				{
					ServerInstance->Log(DEFAULT,"Channel TS for %s changed from %lu to %lu",us->name,ourTS,TS);
					us->age = TS;
					ourTS = TS;
				}
			}
			else
			{
				ServerInstance->Log(DEBUG,"Their channel newer than ours, bouncing their modes");
				std::deque<std::string> params;
				*mode_users[1] = '-';
				for (unsigned int x = 0; x < modectr; x++)
				{
					if (x == 1)
					{
						params.push_back(ConvToStr(us->age));
					}
					params.push_back(mode_users[x]);
				}
				DoOneToMany(this->Instance->Config->ServerName,"FMODE",params);
			}
		}
		return true;
	}

	bool SyncChannelTS(std::string source, std::deque<std::string> &params)
	{
		if (params.size() >= 2)
		{
			chanrec* c = this->Instance->FindChan(params[0]);
			if (c)
			{
				time_t theirTS = atoi(params[1].c_str());
				time_t ourTS = c->age;
				if (ourTS >= theirTS)
				{
					ServerInstance->Log(DEBUG,"Updating timestamp for %s, our timestamp was %lu and theirs is %lu",c->name,ourTS,theirTS);
					c->age = theirTS;
				}
			}
		}
		DoOneToAllButSender(this->Instance->Config->ServerName,"SYNCTS",params,source);
		return true;
	}

	/* NICK command */
	bool IntroduceClient(std::string source, std::deque<std::string> &params)
	{
		if (params.size() < 8)
			return true;
		if (params.size() > 8)
		{
			this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" KILL "+params[1]+" :Invalid client introduction ("+params[1]+"?)");
			return true;
		}
		// NICK age nick host dhost ident +modes ip :gecos
		//       0    1   2     3     4      5   6     7
		time_t age = atoi(params[0].c_str());
		
		/* This used to have a pretty craq'y loop doing the same thing,
		 * now we just let the STL do the hard work (more efficiently)
		 */
		params[5] = params[5].substr(params[5].find_first_not_of('+'));
		
		const char* tempnick = params[1].c_str();
		ServerInstance->Log(DEBUG,"Introduce client %s!%s@%s",tempnick,params[4].c_str(),params[2].c_str());
		
		user_hash::iterator iter = this->Instance->clientlist.find(tempnick);
		
		if (iter != this->Instance->clientlist.end())
		{
			// nick collision
			ServerInstance->Log(DEBUG,"Nick collision on %s!%s@%s: %lu %lu",tempnick,params[4].c_str(),params[2].c_str(),(unsigned long)age,(unsigned long)iter->second->age);
			this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" KILL "+tempnick+" :Nickname collision");
			return true;
		}

		userrec* _new = new userrec(this->Instance);
		this->Instance->clientlist[tempnick] = _new;
		_new->SetFd(FD_MAGIC_NUMBER);
		strlcpy(_new->nick, tempnick,NICKMAX-1);
		strlcpy(_new->host, params[2].c_str(),63);
		strlcpy(_new->dhost, params[3].c_str(),63);
		_new->server = this->Instance->FindServerNamePtr(source.c_str());
		strlcpy(_new->ident, params[4].c_str(),IDENTMAX);
		strlcpy(_new->fullname, params[7].c_str(),MAXGECOS);
		_new->registered = REG_ALL;
		_new->signon = age;
		
		for (std::string::iterator v = params[5].begin(); v != params[5].end(); v++)
			_new->modes[(*v)-65] = 1;

		if (params[6].find_first_of(":") != std::string::npos)
			_new->SetSockAddr(AF_INET6, params[6].c_str(), 0);
		else
			_new->SetSockAddr(AF_INET, params[6].c_str(), 0);

		this->Instance->WriteOpers("*** Client connecting at %s: %s!%s@%s [%s]",_new->server,_new->nick,_new->ident,_new->host, _new->GetIPString());

		params[7] = ":" + params[7];
		DoOneToAllButSender(source,"NICK",params,source);

		// Increment the Source Servers User Count..
		TreeServer* SourceServer = FindServer(source);
		if (SourceServer)
		{
			ServerInstance->Log(DEBUG,"Found source server of %s",_new->nick);
			SourceServer->AddUserCount();
		}

		return true;
	}

	/* Send one or more FJOINs for a channel of users.
	 * If the length of a single line is more than 480-NICKMAX
	 * in length, it is split over multiple lines.
	 */
	void SendFJoins(TreeServer* Current, chanrec* c)
	{
		ServerInstance->Log(DEBUG,"Sending FJOINs to other server for %s",c->name);
		char list[MAXBUF];
		std::string individual_halfops = std::string(":")+this->Instance->Config->ServerName+" FMODE "+c->name+" "+ConvToStr(c->age);
		
		size_t dlen, curlen;
		dlen = curlen = snprintf(list,MAXBUF,":%s FJOIN %s %lu",this->Instance->Config->ServerName,c->name,(unsigned long)c->age);
		int numusers = 0;
		char* ptr = list + dlen;

		CUList *ulist = c->GetUsers();
		std::vector<userrec*> specific_halfop;
		std::vector<userrec*> specific_voice;
		std::string modes = "";
		std::string params = "";

		for (CUList::iterator i = ulist->begin(); i != ulist->end(); i++)
		{
			// The first parameter gets a : before it
			size_t ptrlen = snprintf(ptr, MAXBUF, " %s%s,%s", !numusers ? ":" : "", c->GetAllPrefixChars(i->second), i->second->nick);

			curlen += ptrlen;
			ptr += ptrlen;

			numusers++;

			if (curlen > (480-NICKMAX))
			{
				this->WriteLine(list);
				dlen = curlen = snprintf(list,MAXBUF,":%s FJOIN %s %lu",this->Instance->Config->ServerName,c->name,(unsigned long)c->age);
				ptr = list + dlen;
				ptrlen = 0;
				numusers = 0;
			}
		}

		if (numusers)
			this->WriteLine(list);

                for (BanList::iterator b = c->bans.begin(); b != c->bans.end(); b++)
                {
			modes.append("b");
			params.append(b->data).append(" ");
                }
		this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" FMODE "+c->name+" "+ConvToStr(c->age)+" +"+c->ChanModes(true)+modes+" "+params);
	}

	/* Send G, Q, Z and E lines */
	void SendXLines(TreeServer* Current)
	{
		char data[MAXBUF];
		std::string n = this->Instance->Config->ServerName;
		const char* sn = n.c_str();
		int iterations = 0;
		/* Yes, these arent too nice looking, but they get the job done */
		for (std::vector<ZLine>::iterator i = Instance->XLines->zlines.begin(); i != Instance->XLines->zlines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Z %s %s %lu %lu :%s",sn,i->ipaddr,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<QLine>::iterator i = Instance->XLines->qlines.begin(); i != Instance->XLines->qlines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Q %s %s %lu %lu :%s",sn,i->nick,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<GLine>::iterator i = Instance->XLines->glines.begin(); i != Instance->XLines->glines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE G %s %s %lu %lu :%s",sn,i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<ELine>::iterator i = Instance->XLines->elines.begin(); i != Instance->XLines->elines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE E %s %s %lu %lu :%s",sn,i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<ZLine>::iterator i = Instance->XLines->pzlines.begin(); i != Instance->XLines->pzlines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Z %s %s %lu %lu :%s",sn,i->ipaddr,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<QLine>::iterator i = Instance->XLines->pqlines.begin(); i != Instance->XLines->pqlines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE Q %s %s %lu %lu :%s",sn,i->nick,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<GLine>::iterator i = Instance->XLines->pglines.begin(); i != Instance->XLines->pglines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE G %s %s %lu %lu :%s",sn,i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
		for (std::vector<ELine>::iterator i = Instance->XLines->pelines.begin(); i != Instance->XLines->pelines.end(); i++, iterations++)
		{
			snprintf(data,MAXBUF,":%s ADDLINE E %s %s %lu %lu :%s",sn,i->hostmask,i->source,(unsigned long)i->set_time,(unsigned long)i->duration,i->reason);
			this->WriteLine(data);
		}
	}

	/* Send channel modes and topics */
	void SendChannelModes(TreeServer* Current)
	{
		char data[MAXBUF];
		std::deque<std::string> list;
		int iterations = 0;
		std::string n = this->Instance->Config->ServerName;
		const char* sn = n.c_str();
		for (chan_hash::iterator c = this->Instance->chanlist.begin(); c != this->Instance->chanlist.end(); c++, iterations++)
		{
			SendFJoins(Current, c->second);
			if (*c->second->topic)
			{
				snprintf(data,MAXBUF,":%s FTOPIC %s %lu %s :%s",sn,c->second->name,(unsigned long)c->second->topicset,c->second->setby,c->second->topic);
				this->WriteLine(data);
			}
			FOREACH_MOD_I(this->Instance,I_OnSyncChannel,OnSyncChannel(c->second,(Module*)TreeProtocolModule,(void*)this));
			list.clear();
			c->second->GetExtList(list);
			for (unsigned int j = 0; j < list.size(); j++)
			{
				FOREACH_MOD_I(this->Instance,I_OnSyncChannelMetaData,OnSyncChannelMetaData(c->second,(Module*)TreeProtocolModule,(void*)this,list[j]));
			}
		}
	}

	/* send all users and their oper state/modes */
	void SendUsers(TreeServer* Current)
	{
		char data[MAXBUF];
		std::deque<std::string> list;
		int iterations = 0;
		for (user_hash::iterator u = this->Instance->clientlist.begin(); u != this->Instance->clientlist.end(); u++, iterations++)
		{
			if (u->second->registered == REG_ALL)
			{
				snprintf(data,MAXBUF,":%s NICK %lu %s %s %s %s +%s %s :%s",u->second->server,(unsigned long)u->second->age,u->second->nick,u->second->host,u->second->dhost,u->second->ident,u->second->FormatModes(),u->second->GetIPString(),u->second->fullname);
				this->WriteLine(data);
				if (*u->second->oper)
				{
					this->WriteLine(":"+std::string(u->second->nick)+" OPERTYPE "+std::string(u->second->oper));
				}
				if (*u->second->awaymsg)
				{
					this->WriteLine(":"+std::string(u->second->nick)+" AWAY :"+std::string(u->second->awaymsg));
				}
				FOREACH_MOD_I(this->Instance,I_OnSyncUser,OnSyncUser(u->second,(Module*)TreeProtocolModule,(void*)this));
				list.clear();
				u->second->GetExtList(list);
				for (unsigned int j = 0; j < list.size(); j++)
				{
					FOREACH_MOD_I(this->Instance,I_OnSyncUserMetaData,OnSyncUserMetaData(u->second,(Module*)TreeProtocolModule,(void*)this,list[j]));
				}
			}
		}
	}

	/* This function is called when we want to send a netburst to a local
	 * server. There is a set order we must do this, because for example
	 * users require their servers to exist, and channels require their
	 * users to exist. You get the idea.
	 */
	void DoBurst(TreeServer* s)
	{
		std::string burst = "BURST "+ConvToStr(time(NULL));
		std::string endburst = "ENDBURST";
		// Because by the end of the netburst, it  could be gone!
		std::string name = s->GetName();
		this->Instance->WriteOpers("*** Bursting to \2"+name+"\2.");
		this->WriteLine(burst);
		/* send our version string */
		this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" VERSION :"+this->Instance->GetVersionString());
		/* Send server tree */
		this->SendServers(TreeRoot,s,1);
		/* Send users and their oper status */
		this->SendUsers(s);
		/* Send everything else (channel modes, xlines etc) */
		this->SendChannelModes(s);
		this->SendXLines(s);		
		FOREACH_MOD_I(this->Instance,I_OnSyncOtherMetaData,OnSyncOtherMetaData((Module*)TreeProtocolModule,(void*)this));
		this->WriteLine(endburst);
		this->Instance->WriteOpers("*** Finished bursting to \2"+name+"\2.");
	}

	/* This function is called when we receive data from a remote
	 * server. We buffer the data in a std::string (it doesnt stay
	 * there for long), reading using InspSocket::Read() which can
	 * read up to 16 kilobytes in one operation.
	 *
	 * IF THIS FUNCTION RETURNS FALSE, THE CORE CLOSES AND DELETES
	 * THE SOCKET OBJECT FOR US.
	 */
	virtual bool OnDataReady()
	{
		char* data = this->Read();
		/* Check that the data read is a valid pointer and it has some content */
		if (data && *data)
		{
			this->in_buffer.append(data);
			/* While there is at least one new line in the buffer,
			 * do something useful (we hope!) with it.
			 */
			while (in_buffer.find("\n") != std::string::npos)
			{
				std::string ret = in_buffer.substr(0,in_buffer.find("\n")-1);
				in_buffer = in_buffer.substr(in_buffer.find("\n")+1,in_buffer.length()-in_buffer.find("\n"));
				if (ret.find("\r") != std::string::npos)
					ret = in_buffer.substr(0,in_buffer.find("\r")-1);
				/* Process this one, abort if it
				 * didnt return true.
				 */
				if (this->ctx_in)
				{
					char out[1024];
					char result[1024];
					memset(result,0,1024);
					memset(out,0,1024);
					ServerInstance->Log(DEBUG,"Original string '%s'",ret.c_str());
					/* ERROR + CAPAB is still allowed unencryped */
					if ((ret.substr(0,7) != "ERROR :") && (ret.substr(0,6) != "CAPAB "))
					{
						int nbytes = from64tobits(out, ret.c_str(), 1024);
						if ((nbytes > 0) && (nbytes < 1024))
						{
							ServerInstance->Log(DEBUG,"m_spanningtree: decrypt %d bytes",nbytes);
							ctx_in->Decrypt(out, result, nbytes, 0);
							for (int t = 0; t < nbytes; t++)
								if (result[t] == '\7') result[t] = 0;
							ret = result;
						}
					}
				}
				if (!this->ProcessLine(ret))
				{
					ServerInstance->Log(DEBUG,"ProcessLine says no!");
					return false;
				}
			}
			return true;
		}
		/* EAGAIN returns an empty but non-NULL string, so this
		 * evaluates to TRUE for EAGAIN but to FALSE for EOF.
		 */
		return (data && !*data);
	}

	int WriteLine(std::string line)
	{
		ServerInstance->Log(DEBUG,"OUT: %s",line.c_str());
		if (this->ctx_out)
		{
			char result[10240];
			char result64[10240];
			if (this->keylength)
			{
				// pad it to the key length
				int n = this->keylength - (line.length() % this->keylength);
				if (n)
				{
					ServerInstance->Log(DEBUG,"Append %d chars to line to make it %d long from %d, key length %d",n,n+line.length(),line.length(),this->keylength);
					line.append(n,'\7');
				}
			}
			unsigned int ll = line.length();
			ctx_out->Encrypt(line.c_str(), result, ll, 0);
			to64frombits((unsigned char*)result64,(unsigned char*)result,ll);
			line = result64;
			//int from64tobits(char *out, const char *in, int maxlen);
		}
		return this->Write(line + "\r\n");
	}

	/* Handle ERROR command */
	bool Error(std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return false;
		this->Instance->WriteOpers("*** ERROR from %s: %s",(InboundServerName != "" ? InboundServerName.c_str() : myhost.c_str()),params[0].c_str());
		/* we will return false to cause the socket to close. */
		return false;
	}

	bool Stats(std::string prefix, std::deque<std::string> &params)
	{
		/* Get the reply to a STATS query if it matches this servername,
		 * and send it back as a load of PUSH queries
		 */
		if (params.size() > 1)
		{
			if (this->Instance->MatchText(this->Instance->Config->ServerName, params[1]))
			{
				/* It's for our server */
				string_list results;
				userrec* source = this->Instance->FindNick(prefix);
				if (source)
				{
					std::deque<std::string> par;
					par.push_back(prefix);
					par.push_back("");
					DoStats(this->Instance, *(params[0].c_str()), source, results);
					for (size_t i = 0; i < results.size(); i++)
					{
						par[1] = "::" + results[i];
						DoOneToOne(this->Instance->Config->ServerName, "PUSH",par, source->server);
					}
				}
			}
			else
			{
				/* Pass it on */
				userrec* source = this->Instance->FindNick(prefix);
				if (source)
					DoOneToOne(prefix, "STATS", params, params[1]);
			}
		}
		return true;
	}


	/* Because the core won't let users or even SERVERS set +o,
	 * we use the OPERTYPE command to do this.
	 */
	bool OperType(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() != 1)
		{
			ServerInstance->Log(DEBUG,"Received invalid oper type from %s",prefix.c_str());
			return true;
		}
		std::string opertype = params[0];
		userrec* u = this->Instance->FindNick(prefix);
		if (u)
		{
			u->modes[UM_OPERATOR] = 1;
			strlcpy(u->oper,opertype.c_str(),NICKMAX-1);
			DoOneToAllButSender(u->nick,"OPERTYPE",params,u->server);
		}
		return true;
	}

	/* Because Andy insists that services-compatible servers must
	 * implement SVSNICK and SVSJOIN, that's exactly what we do :p
	 */
	bool ForceNick(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 3)
			return true;

		userrec* u = this->Instance->FindNick(params[0]);

		if (u)
		{
			DoOneToAllButSender(prefix,"SVSNICK",params,prefix);
			if (IS_LOCAL(u))
			{
				std::deque<std::string> par;
				par.push_back(params[1]);
				/* This is not required as one is sent in OnUserPostNick below
				 */
				//DoOneToMany(u->nick,"NICK",par);
				if (!u->ForceNickChange(params[1].c_str()))
				{
					userrec::QuitUser(this->Instance, u, "Nickname collision");
					return true;
				}
				u->age = atoi(params[2].c_str());
			}
		}
		return true;
	}

	bool ServiceJoin(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 2)
			return true;

		userrec* u = this->Instance->FindNick(params[0]);

		if (u)
		{
			chanrec::JoinUser(this->Instance, u, params[1].c_str(), false);
			DoOneToAllButSender(prefix,"SVSJOIN",params,prefix);
		}
		return true;
	}

	bool RemoteRehash(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return false;

		std::string servermask = params[0];

		if (this->Instance->MatchText(this->Instance->Config->ServerName,servermask))
		{
			this->Instance->WriteOpers("*** Remote rehash initiated from server \002"+prefix+"\002.");
			this->Instance->RehashServer();
			ReadConfiguration(false);
		}
		DoOneToAllButSender(prefix,"REHASH",params,prefix);
		return true;
	}

	bool RemoteKill(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() != 2)
			return true;

		std::string nick = params[0];
		userrec* u = this->Instance->FindNick(prefix);
		userrec* who = this->Instance->FindNick(nick);

		if (who)
		{
			/* Prepend kill source, if we don't have one */
			std::string sourceserv = prefix;
			if (u)
			{
				sourceserv = u->server;
			}
			if (*(params[1].c_str()) != '[')
			{
				params[1] = "[" + sourceserv + "] Killed (" + params[1] +")";
			}
			std::string reason = params[1];
			params[1] = ":" + params[1];
			DoOneToAllButSender(prefix,"KILL",params,sourceserv);
			who->Write(":%s KILL %s :%s (%s)", sourceserv.c_str(), who->nick, sourceserv.c_str(), reason.c_str());
			userrec::QuitUser(this->Instance,who,reason);
		}
		return true;
	}

	bool LocalPong(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		if (params.size() == 1)
		{
			TreeServer* ServerSource = FindServer(prefix);
			if (ServerSource)
			{
				ServerSource->SetPingFlag();
			}
		}
		else
		{
			std::string forwardto = params[1];
			if (forwardto == this->Instance->Config->ServerName)
			{
				/*
				 * this is a PONG for us
				 * if the prefix is a user, check theyre local, and if they are,
				 * dump the PONG reply back to their fd. If its a server, do nowt.
				 * Services might want to send these s->s, but we dont need to yet.
				 */
				userrec* u = this->Instance->FindNick(prefix);

				if (u)
				{
					u->WriteServ("PONG %s %s",params[0].c_str(),params[1].c_str());
				}
			}
			else
			{
				// not for us, pass it on :)
				DoOneToOne(prefix,"PONG",params,forwardto);
			}
		}

		return true;
	}
	
	bool MetaData(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 3)
			return true;

		TreeServer* ServerSource = FindServer(prefix);

		if (ServerSource)
		{
			if (params[0] == "*")
			{
				FOREACH_MOD_I(this->Instance,I_OnDecodeMetaData,OnDecodeMetaData(TYPE_OTHER,NULL,params[1],params[2]));
			}
			else if (*(params[0].c_str()) == '#')
			{
				chanrec* c = this->Instance->FindChan(params[0]);
				if (c)
				{
					FOREACH_MOD_I(this->Instance,I_OnDecodeMetaData,OnDecodeMetaData(TYPE_CHANNEL,c,params[1],params[2]));
				}
			}
			else if (*(params[0].c_str()) != '#')
			{
				userrec* u = this->Instance->FindNick(params[0]);
				if (u)
				{
					FOREACH_MOD_I(this->Instance,I_OnDecodeMetaData,OnDecodeMetaData(TYPE_USER,u,params[1],params[2]));
				}
			}
		}

		params[2] = ":" + params[2];
		DoOneToAllButSender(prefix,"METADATA",params,prefix);
		return true;
	}

	bool ServerVersion(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		TreeServer* ServerSource = FindServer(prefix);

		if (ServerSource)
		{
			ServerSource->SetVersion(params[0]);
		}
		params[0] = ":" + params[0];
		DoOneToAllButSender(prefix,"VERSION",params,prefix);
		return true;
	}

	bool ChangeHost(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		userrec* u = this->Instance->FindNick(prefix);

		if (u)
		{
			u->ChangeDisplayedHost(params[0].c_str());
			DoOneToAllButSender(prefix,"FHOST",params,u->server);
		}
		return true;
	}

	bool AddLine(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 6)
			return true;

		bool propogate = false;

		switch (*(params[0].c_str()))
		{
			case 'Z':
				propogate = ServerInstance->XLines->add_zline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				ServerInstance->XLines->zline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'Q':
				propogate = ServerInstance->XLines->add_qline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				ServerInstance->XLines->qline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'E':
				propogate = ServerInstance->XLines->add_eline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				ServerInstance->XLines->eline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'G':
				propogate = ServerInstance->XLines->add_gline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
				ServerInstance->XLines->gline_set_creation_time(params[1].c_str(), atoi(params[3].c_str()));
			break;
			case 'K':
				propogate = ServerInstance->XLines->add_kline(atoi(params[4].c_str()), params[2].c_str(), params[5].c_str(), params[1].c_str());
			break;
			default:
				/* Just in case... */
				this->Instance->WriteOpers("*** \2WARNING\2: Invalid xline type '"+params[0]+"' sent by server "+prefix+", ignored!");
				propogate = false;
			break;
		}

		/* Send it on its way */
		if (propogate)
		{
			if (atoi(params[4].c_str()))
			{
				this->Instance->WriteOpers("*** %s Added %cLINE on %s to expire in %lu seconds (%s).",prefix.c_str(),*(params[0].c_str()),params[1].c_str(),atoi(params[4].c_str()),params[5].c_str());
			}
			else
			{
				this->Instance->WriteOpers("*** %s Added permenant %cLINE on %s (%s).",prefix.c_str(),*(params[0].c_str()),params[1].c_str(),params[5].c_str());
			}
			params[5] = ":" + params[5];
			DoOneToAllButSender(prefix,"ADDLINE",params,prefix);
		}
		if (!this->bursting)
		{
			ServerInstance->Log(DEBUG,"Applying lines...");
			ServerInstance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
		}
		return true;
	}

	bool ChangeName(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		userrec* u = this->Instance->FindNick(prefix);

		if (u)
		{
			u->ChangeName(params[0].c_str());
			params[0] = ":" + params[0];
			DoOneToAllButSender(prefix,"FNAME",params,u->server);
		}
		return true;
	}

	bool Whois(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		ServerInstance->Log(DEBUG,"In IDLE command");
		userrec* u = this->Instance->FindNick(prefix);

		if (u)
		{
			ServerInstance->Log(DEBUG,"USER EXISTS: %s",u->nick);
			// an incoming request
			if (params.size() == 1)
			{
				userrec* x = this->Instance->FindNick(params[0]);
				if ((x) && (IS_LOCAL(x)))
				{
					userrec* x = this->Instance->FindNick(params[0]);
					ServerInstance->Log(DEBUG,"Got IDLE");
					char signon[MAXBUF];
					char idle[MAXBUF];
					ServerInstance->Log(DEBUG,"Sending back IDLE 3");
					snprintf(signon,MAXBUF,"%lu",(unsigned long)x->signon);
					snprintf(idle,MAXBUF,"%lu",(unsigned long)abs((x->idle_lastmsg)-time(NULL)));
					std::deque<std::string> par;
					par.push_back(prefix);
					par.push_back(signon);
					par.push_back(idle);
					// ours, we're done, pass it BACK
					DoOneToOne(params[0],"IDLE",par,u->server);
				}
				else
				{
					// not ours pass it on
					DoOneToOne(prefix,"IDLE",params,x->server);
				}
			}
			else if (params.size() == 3)
			{
				std::string who_did_the_whois = params[0];
				userrec* who_to_send_to = this->Instance->FindNick(who_did_the_whois);
				if ((who_to_send_to) && (IS_LOCAL(who_to_send_to)))
				{
					ServerInstance->Log(DEBUG,"Got final IDLE");
					// an incoming reply to a whois we sent out
					std::string nick_whoised = prefix;
					unsigned long signon = atoi(params[1].c_str());
					unsigned long idle = atoi(params[2].c_str());
					if ((who_to_send_to) && (IS_LOCAL(who_to_send_to)))
						do_whois(this->Instance,who_to_send_to,u,signon,idle,nick_whoised.c_str());
				}
				else
				{
					// not ours, pass it on
					DoOneToOne(prefix,"IDLE",params,who_to_send_to->server);
				}
			}
		}
		return true;
	}

	bool Push(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 2)
			return true;

		userrec* u = this->Instance->FindNick(params[0]);

		if (!u)
			return true;

		if (IS_LOCAL(u))
		{
			u->Write(params[1]);
		}
		else
		{
			// continue the raw onwards
			params[1] = ":" + params[1];
			DoOneToOne(prefix,"PUSH",params,u->server);
		}
		return true;
	}

	bool Time(std::string prefix, std::deque<std::string> &params)
	{
		// :source.server TIME remote.server sendernick
		// :remote.server TIME source.server sendernick TS
		if (params.size() == 2)
		{
			// someone querying our time?
			if (this->Instance->Config->ServerName == params[0])
			{
				userrec* u = this->Instance->FindNick(params[1]);
				if (u)
				{
					char curtime[256];
					snprintf(curtime,256,"%lu",(unsigned long)time(NULL));
					params.push_back(curtime);
					params[0] = prefix;
					DoOneToOne(this->Instance->Config->ServerName,"TIME",params,params[0]);
				}
			}
			else
			{
				// not us, pass it on
				userrec* u = this->Instance->FindNick(params[1]);
				if (u)
					DoOneToOne(prefix,"TIME",params,params[0]);
			}
		}
		else if (params.size() == 3)
		{
			// a response to a previous TIME
			userrec* u = this->Instance->FindNick(params[1]);
			if ((u) && (IS_LOCAL(u)))
			{
			time_t rawtime = atol(params[2].c_str());
			struct tm * timeinfo;
			timeinfo = localtime(&rawtime);
				char tms[26];
				snprintf(tms,26,"%s",asctime(timeinfo));
				tms[24] = 0;
			u->WriteServ("391 %s %s :%s",u->nick,prefix.c_str(),tms);
			}
			else
			{
				if (u)
					DoOneToOne(prefix,"TIME",params,u->server);
			}
		}
		return true;
	}
	
	bool LocalPing(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 1)
			return true;

		if (params.size() == 1)
		{
			std::string stufftobounce = params[0];
			this->WriteLine(std::string(":")+this->Instance->Config->ServerName+" PONG "+stufftobounce);
			return true;
		}
		else
		{
			std::string forwardto = params[1];
			if (forwardto == this->Instance->Config->ServerName)
			{
				// this is a ping for us, send back PONG to the requesting server
				params[1] = params[0];
				params[0] = forwardto;
				DoOneToOne(forwardto,"PONG",params,params[1]);
			}
			else
			{
				// not for us, pass it on :)
				DoOneToOne(prefix,"PING",params,forwardto);
			}
			return true;
		}
	}

	bool RemoteServer(std::string prefix, std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;

		std::string servername = params[0];
		std::string password = params[1];
		// hopcount is not used for a remote server, we calculate this ourselves
		std::string description = params[3];
		TreeServer* ParentOfThis = FindServer(prefix);

		if (!ParentOfThis)
		{
			this->WriteLine("ERROR :Protocol error - Introduced remote server from unknown server "+prefix);
			return false;
		}
		TreeServer* CheckDupe = FindServer(servername);
		if (CheckDupe)
		{
			this->WriteLine("ERROR :Server "+servername+" already exists!");
			this->Instance->WriteOpers("*** Server connection from \2"+servername+"\2 denied, already exists");
			return false;
		}
		TreeServer* Node = new TreeServer(this->Instance,servername,description,ParentOfThis,NULL);
		ParentOfThis->AddChild(Node);
		params[3] = ":" + params[3];
		DoOneToAllButSender(prefix,"SERVER",params,prefix);
		this->Instance->WriteOpers("*** Server \002"+prefix+"\002 introduced server \002"+servername+"\002 ("+description+")");
		return true;
	}

	bool Outbound_Reply_Server(std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;

		irc::string servername = params[0].c_str();
		std::string sname = params[0];
		std::string password = params[1];
		int hops = atoi(params[2].c_str());

		if (hops)
		{
			this->WriteLine("ERROR :Server too far away for authentication");
			this->Instance->WriteOpers("*** Server connection from \2"+sname+"\2 denied, server is too far away for authentication");
			return false;
		}
		std::string description = params[3];
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if ((x->Name == servername) && (x->RecvPass == password))
			{
				TreeServer* CheckDupe = FindServer(sname);
				if (CheckDupe)
				{
					this->WriteLine("ERROR :Server "+sname+" already exists on server "+CheckDupe->GetParent()->GetName()+"!");
					this->Instance->WriteOpers("*** Server connection from \2"+sname+"\2 denied, already exists on server "+CheckDupe->GetParent()->GetName());
					return false;
				}
				// Begin the sync here. this kickstarts the
				// other side, waiting in WAIT_AUTH_2 state,
				// into starting their burst, as it shows
				// that we're happy.
				this->LinkState = CONNECTED;
				// we should add the details of this server now
				// to the servers tree, as a child of the root
				// node.
				TreeServer* Node = new TreeServer(this->Instance,sname,description,TreeRoot,this);
				TreeRoot->AddChild(Node);
				params[3] = ":" + params[3];
				DoOneToAllButSender(TreeRoot->GetName(),"SERVER",params,sname);
				this->bursting = true;
				this->DoBurst(Node);
				return true;
			}
		}
		this->WriteLine("ERROR :Invalid credentials");
		this->Instance->WriteOpers("*** Server connection from \2"+sname+"\2 denied, invalid link credentials");
		return false;
	}

	bool Inbound_Server(std::deque<std::string> &params)
	{
		if (params.size() < 4)
			return false;

		irc::string servername = params[0].c_str();
		std::string sname = params[0];
		std::string password = params[1];
		int hops = atoi(params[2].c_str());

		if (hops)
		{
			this->WriteLine("ERROR :Server too far away for authentication");
			this->Instance->WriteOpers("*** Server connection from \2"+sname+"\2 denied, server is too far away for authentication");
			return false;
		}
		std::string description = params[3];
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if ((x->Name == servername) && (x->RecvPass == password))
			{
				TreeServer* CheckDupe = FindServer(sname);
				if (CheckDupe)
				{
					this->WriteLine("ERROR :Server "+sname+" already exists on server "+CheckDupe->GetParent()->GetName()+"!");
					this->Instance->WriteOpers("*** Server connection from \2"+sname+"\2 denied, already exists on server "+CheckDupe->GetParent()->GetName());
					return false;
				}
				/* If the config says this link is encrypted, but the remote side
				 * hasnt bothered to send the AES command before SERVER, then we
				 * boot them off as we MUST have this connection encrypted.
				 */
				if ((x->EncryptionKey != "") && (!this->ctx_in))
				{
					this->WriteLine("ERROR :This link requires AES encryption to be enabled. Plaintext connection refused.");
					this->Instance->WriteOpers("*** Server connection from \2"+sname+"\2 denied, remote server did not enable AES.");
					return false;
				}
				this->Instance->WriteOpers("*** Verified incoming server connection from \002"+sname+"\002["+(x->HiddenFromStats ? "<hidden>" : this->GetIP())+"] ("+description+")");
				this->InboundServerName = sname;
				this->InboundDescription = description;
				// this is good. Send our details: Our server name and description and hopcount of 0,
				// along with the sendpass from this block.
				this->WriteLine(std::string("SERVER ")+this->Instance->Config->ServerName+" "+x->SendPass+" 0 :"+this->Instance->Config->ServerDesc);
				// move to the next state, we are now waiting for THEM.
				this->LinkState = WAIT_AUTH_2;
				return true;
			}
		}
		this->WriteLine("ERROR :Invalid credentials");
		this->Instance->WriteOpers("*** Server connection from \2"+sname+"\2 denied, invalid link credentials");
		return false;
	}

	void Split(std::string line, std::deque<std::string> &n)
	{
		n.clear();
		irc::tokenstream tokens(line);
		std::string param;
		while ((param = tokens.GetToken()) != "")
			n.push_back(param);
		return;
	}

	bool ProcessLine(std::string line)
	{
		std::deque<std::string> params;
		irc::string command;
		std::string prefix;
		
		if (line.empty())
			return true;
		
		line = line.substr(0, line.find_first_of("\r\n"));
		
		ServerInstance->Log(DEBUG,"IN: %s", line.c_str());
		
		this->Split(line.c_str(),params);
			
		if ((params[0][0] == ':') && (params.size() > 1))
		{
			prefix = params[0].substr(1);
			params.pop_front();
		}

		command = params[0].c_str();
		params.pop_front();

		if ((!this->ctx_in) && (command == "AES"))
		{
			std::string sserv = params[0];
			for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
			{
				if ((x->EncryptionKey != "") && (x->Name == sserv))
				{
					this->InitAES(x->EncryptionKey,sserv);
				}
			}

			return true;
		}
		else if ((this->ctx_in) && (command == "AES"))
		{
			this->Instance->WriteOpers("*** \2AES\2: Encryption already enabled on this connection yet %s is trying to enable it twice!",params[0].c_str());
		}

		switch (this->LinkState)
		{
			TreeServer* Node;
			
			case WAIT_AUTH_1:
				// Waiting for SERVER command from remote server. Server initiating
				// the connection sends the first SERVER command, listening server
				// replies with theirs if its happy, then if the initiator is happy,
				// it starts to send its net sync, which starts the merge, otherwise
				// it sends an ERROR.
				if (command == "PASS")
				{
					/* Silently ignored */
				}
				else if (command == "SERVER")
				{
					return this->Inbound_Server(params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "USER")
				{
					this->WriteLine("ERROR :Client connections to this port are prohibited.");
					return false;
				}
				else if (command == "CAPAB")
				{
					return this->Capab(params);
				}
				else if ((command == "U") || (command == "S"))
				{
					this->WriteLine("ERROR :Cannot use the old-style mesh linking protocol with m_spanningtree.so!");
					return false;
				}
				else
				{
					this->WriteLine("ERROR :Invalid command in negotiation phase.");
					return false;
				}
			break;
			case WAIT_AUTH_2:
				// Waiting for start of other side's netmerge to say they liked our
				// password.
				if (command == "SERVER")
				{
					// cant do this, they sent it to us in the WAIT_AUTH_1 state!
					// silently ignore.
					return true;
				}
				else if ((command == "U") || (command == "S"))
				{
					this->WriteLine("ERROR :Cannot use the old-style mesh linking protocol with m_spanningtree.so!");
					return false;
				}
				else if (command == "BURST")
				{
					if (params.size())
					{
						/* If a time stamp is provided, try and check syncronization */
						time_t THEM = atoi(params[0].c_str());
						long delta = THEM-time(NULL);
						if ((delta < -600) || (delta > 600))
						{
							this->Instance->WriteOpers("*** \2ERROR\2: Your clocks are out by %d seconds (this is more than ten minutes). Link aborted, \2PLEASE SYNC YOUR CLOCKS!\2",abs(delta));
							this->WriteLine("ERROR :Your clocks are out by "+ConvToStr(abs(delta))+" seconds (this is more than ten minutes). Link aborted, PLEASE SYNC YOUR CLOCKS!");
							return false;
						}
						else if ((delta < -60) || (delta > 60))
						{
							this->Instance->WriteOpers("*** \2WARNING\2: Your clocks are out by %d seconds, please consider synching your clocks.",abs(delta));
						}
					}
					this->LinkState = CONNECTED;
					Node = new TreeServer(this->Instance,InboundServerName,InboundDescription,TreeRoot,this);
					TreeRoot->AddChild(Node);
					params.clear();
					params.push_back(InboundServerName);
					params.push_back("*");
					params.push_back("1");
					params.push_back(":"+InboundDescription);
					DoOneToAllButSender(TreeRoot->GetName(),"SERVER",params,InboundServerName);
					this->bursting = true;
					this->DoBurst(Node);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "CAPAB")
				{
					return this->Capab(params);
				}
				
			break;
			case LISTENER:
				this->WriteLine("ERROR :Internal error -- listening socket accepted its own descriptor!!!");
				return false;
			break;
			case CONNECTING:
				if (command == "SERVER")
				{
					// another server we connected to, which was in WAIT_AUTH_1 state,
					// has just sent us their credentials. If we get this far, theyre
					// happy with OUR credentials, and they are now in WAIT_AUTH_2 state.
					// if we're happy with this, we should send our netburst which
					// kickstarts the merge.
					return this->Outbound_Reply_Server(params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
			break;
			case CONNECTED:
				// This is the 'authenticated' state, when all passwords
				// have been exchanged and anything past this point is taken
				// as gospel.
				
				if (prefix != "")
				{
					std::string direction = prefix;
					userrec* t = this->Instance->FindNick(prefix);
					if (t)
					{
						direction = t->server;
					}
					TreeServer* route_back_again = BestRouteTo(direction);
					if ((!route_back_again) || (route_back_again->GetSocket() != this))
					{
						if (route_back_again)
							ServerInstance->Log(DEBUG,"Protocol violation: Fake direction in command '%s' from connection '%s'",line.c_str(),this->GetName().c_str());
						return true;
					}

					/* Fix by brain:
					 * When there is activity on the socket, reset the ping counter so
					 * that we're not wasting bandwidth pinging an active server.
					 */ 
					route_back_again->SetNextPingTime(time(NULL) + 120);
					route_back_again->SetPingFlag();
				}
				
				if (command == "SVSMODE")
				{
					/* Services expects us to implement
					 * SVSMODE. In inspircd its the same as
					 * MODE anyway.
					 */
					command = "MODE";
				}
				std::string target = "";
				/* Yes, know, this is a mess. Its reasonably fast though as we're
				 * working with std::string here.
				 */
				if ((command == "NICK") && (params.size() > 1))
				{
					return this->IntroduceClient(prefix,params);
				}
				else if (command == "FJOIN")
				{
					return this->ForceJoin(prefix,params);
				}
				else if (command == "STATS")
				{
					return this->Stats(prefix, params);
				}
				else if (command == "SERVER")
				{
					return this->RemoteServer(prefix,params);
				}
				else if (command == "ERROR")
				{
					return this->Error(params);
				}
				else if (command == "OPERTYPE")
				{
					return this->OperType(prefix,params);
				}
				else if (command == "FMODE")
				{
					return this->ForceMode(prefix,params);
				}
				else if (command == "KILL")
				{
					return this->RemoteKill(prefix,params);
				}
				else if (command == "FTOPIC")
				{
					return this->ForceTopic(prefix,params);
				}
				else if (command == "REHASH")
				{
					return this->RemoteRehash(prefix,params);
				}
				else if (command == "METADATA")
				{
					return this->MetaData(prefix,params);
				}
				else if (command == "PING")
				{
					/*
					 * We just got a ping from a server that's bursting.
					 * This can't be right, so set them to not bursting, and
					 * apply their lines.
					 */
					if (this->bursting)
					{
						this->bursting = false;
						ServerInstance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
					}
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->LocalPing(prefix,params);
				}
				else if (command == "PONG")
				{
					/*
					 * We just got a pong from a server that's bursting.
					 * This can't be right, so set them to not bursting, and
					 * apply their lines.
					 */
					if (this->bursting)
					{
						this->bursting = false;
						ServerInstance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
					}
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->LocalPong(prefix,params);
				}
				else if (command == "VERSION")
				{
					return this->ServerVersion(prefix,params);
				}
				else if (command == "FHOST")
				{
					return this->ChangeHost(prefix,params);
				}
				else if (command == "FNAME")
				{
					return this->ChangeName(prefix,params);
				}
				else if (command == "ADDLINE")
				{
					return this->AddLine(prefix,params);
				}
				else if (command == "SVSNICK")
				{
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->ForceNick(prefix,params);
				}
				else if (command == "IDLE")
				{
					return this->Whois(prefix,params);
				}
				else if (command == "PUSH")
				{
					return this->Push(prefix,params);
				}
				else if (command == "TIME")
				{
					return this->Time(prefix,params);
				}
				else if ((command == "KICK") && (IsServer(prefix)))
				{
					std::string sourceserv = this->myhost;
					if (params.size() == 3)
					{
						userrec* user = this->Instance->FindNick(params[1]);
						chanrec* chan = this->Instance->FindChan(params[0]);
						if (user && chan)
						{
							if (!chan->ServerKickUser(user, params[2].c_str(), false))
								/* Yikes, the channels gone! */
								delete chan;
						}
					}
					if (this->InboundServerName != "")
					{
						sourceserv = this->InboundServerName;
					}
					return DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);
				}
				else if (command == "SVSJOIN")
				{
					if (prefix == "")
					{
						prefix = this->GetName();
					}
					return this->ServiceJoin(prefix,params);
				}
				else if (command == "SQUIT")
				{
					if (params.size() == 2)
					{
						this->Squit(FindServer(params[0]),params[1]);
					}
					return true;
				}
				else if (command == "ENDBURST")
				{
					this->bursting = false;
					ServerInstance->XLines->apply_lines(APPLY_ZLINES|APPLY_GLINES|APPLY_QLINES);
					std::string sourceserv = this->myhost;
					if (this->InboundServerName != "")
					{
						sourceserv = this->InboundServerName;
					}
					this->Instance->WriteOpers("*** Received end of netburst from \2%s\2",sourceserv.c_str());
					return true;
				}
				else
				{
					// not a special inter-server command.
					// Emulate the actual user doing the command,
					// this saves us having a huge ugly parser.
					userrec* who = this->Instance->FindNick(prefix);
					std::string sourceserv = this->myhost;
					if (this->InboundServerName != "")
					{
						sourceserv = this->InboundServerName;
					}
					if (who)
					{
						if ((command == "NICK") && (params.size() > 0))
						{
							/* On nick messages, check that the nick doesnt
							 * already exist here. If it does, kill their copy,
							 * and our copy.
							 */
							userrec* x = this->Instance->FindNick(params[0]);
							if ((x) && (x != who))
							{
								std::deque<std::string> p;
								p.push_back(params[0]);
								p.push_back("Nickname collision ("+prefix+" -> "+params[0]+")");
								DoOneToMany(this->Instance->Config->ServerName,"KILL",p);
								p.clear();
								p.push_back(prefix);
								p.push_back("Nickname collision");
								DoOneToMany(this->Instance->Config->ServerName,"KILL",p);
								userrec::QuitUser(this->Instance,x,"Nickname collision ("+prefix+" -> "+params[0]+")");
								userrec* y = this->Instance->FindNick(prefix);
								if (y)
								{
									userrec::QuitUser(this->Instance,y,"Nickname collision");
								}
								return DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);
							}
						}
						// its a user
						target = who->server;
						const char* strparams[127];
						for (unsigned int q = 0; q < params.size(); q++)
						{
							strparams[q] = params[q].c_str();
						}
						if (!this->Instance->CallCommandHandler(command.c_str(), strparams, params.size(), who))
						{
							this->WriteLine("ERROR :Unrecognised command '"+std::string(command.c_str())+"' -- possibly loaded mismatched modules");
							return false;
						}
					}
					else
					{
						// its not a user. Its either a server, or somethings screwed up.
						if (IsServer(prefix))
						{
							target = this->Instance->Config->ServerName;
						}
						else
						{
							ServerInstance->Log(DEBUG,"Command with unknown origin '%s'",prefix.c_str());
							return true;
						}
					}
					return DoOneToAllButSenderRaw(line,sourceserv,prefix,command,params);

				}
				return true;
			break;
		}
		return true;
	}

	virtual std::string GetName()
	{
		std::string sourceserv = this->myhost;
		if (this->InboundServerName != "")
		{
			sourceserv = this->InboundServerName;
		}
		return sourceserv;
	}

	virtual void OnTimeout()
	{
		if (this->LinkState == CONNECTING)
		{
			this->Instance->WriteOpers("*** CONNECT: Connection to \002"+myhost+"\002 timed out.");
		}
	}

	virtual void OnClose()
	{
		// Connection closed.
		// If the connection is fully up (state CONNECTED)
		// then propogate a netsplit to all peers.
		std::string quitserver = this->myhost;
		if (this->InboundServerName != "")
		{
			quitserver = this->InboundServerName;
		}
		TreeServer* s = FindServer(quitserver);
		if (s)
		{
			Squit(s,"Remote host closed the connection");
		}
		this->Instance->WriteOpers("Server '\2%s\2' closed the connection.",quitserver.c_str());
	}

	virtual int OnIncomingConnection(int newsock, char* ip)
	{
		/* To prevent anyone from attempting to flood opers/DDoS by connecting to the server port,
		 * or discovering if this port is the server port, we don't allow connections from any
		 * IPs for which we don't have a link block.
		 */
		bool found = false;

		found = (std::find(ValidIPs.begin(), ValidIPs.end(), ip) != ValidIPs.end());
		if (!found)
		{
			for (vector<std::string>::iterator i = ValidIPs.begin(); i != ValidIPs.end(); i++)
				if (MatchCIDR(ip, (*i).c_str()))
					found = true;

			if (!found)
			{
				this->Instance->WriteOpers("Server connection from %s denied (no link blocks with that IP address)", ip);
				close(newsock);
				return false;
			}
		}
		TreeSocket* s = new TreeSocket(this->Instance, newsock, ip);
		this->Instance->AddSocket(s);
		return true;
	}
};

/** This class is used to resolve server hostnames during /connect and autoconnect.
 * As of 1.1, the resolver system is seperated out from InspSocket, so we must do this
 * resolver step first ourselves if we need it. This is totally nonblocking, and will
 * callback to OnLookupComplete or OnError when completed. Once it has completed we
 * will have an IP address which we can then use to continue our connection.
 */
class ServernameResolver : public Resolver
{       
 private:
	/** A copy of the Link tag info for what we're connecting to.
	 * We take a copy, rather than using a pointer, just in case the
	 * admin takes the tag away and rehashes while the domain is resolving.
	 */
	Link MyLink;
 public: 
	ServernameResolver(InspIRCd* Instance, const std::string &hostname, Link x) : Resolver(Instance, hostname, DNS_QUERY_FORWARD), MyLink(x)
	{
		/* Nothing in here, folks */
	}

	void OnLookupComplete(const std::string &result)
	{
		/* Initiate the connection, now that we have an IP to use.
		 * Passing a hostname directly to InspSocket causes it to
		 * just bail and set its FD to -1.
		 */
		TreeServer* CheckDupe = FindServer(MyLink.Name.c_str());
		if (!CheckDupe) /* Check that nobody tried to connect it successfully while we were resolving */
		{
			TreeSocket* newsocket = new TreeSocket(ServerInstance, result,MyLink.Port,false,10,MyLink.Name.c_str());
			if (newsocket->GetFd() > -1)
			{
				/* We're all OK */
				ServerInstance->AddSocket(newsocket);
			}
			else
			{
				/* Something barfed, show the opers */
				ServerInstance->WriteOpers("*** CONNECT: Error connecting \002%s\002: %s.",MyLink.Name.c_str(),strerror(errno));
				delete newsocket;
			}
		}
	}

	void OnError(ResolverError e, const std::string &errormessage)
	{
		/* Ooops! */
		ServerInstance->WriteOpers("*** CONNECT: Error connecting \002%s\002: Unable to resolve hostname - %s",MyLink.Name.c_str(),errormessage.c_str());
	}
};

class SecurityIPResolver : public Resolver
{
 private:
	Link MyLink;
 public:
	SecurityIPResolver(InspIRCd* Instance, const std::string &hostname, Link x) : Resolver(Instance, hostname, DNS_QUERY_FORWARD), MyLink(x)
	{
	}

	void OnLookupComplete(const std::string &result)
	{
		ServerInstance->Log(DEBUG,"Security IP cache: Adding IP address '%s' for Link '%s'",result.c_str(),MyLink.Name.c_str());
		ValidIPs.push_back(result);
	}

	void OnError(ResolverError e, const std::string &errormessage)
	{
		ServerInstance->Log(DEBUG,"Could not resolve IP associated with Link '%s': %s",MyLink.Name.c_str(),errormessage.c_str());
	}
};

void AddThisServer(TreeServer* server, std::deque<TreeServer*> &list)
{
	for (unsigned int c = 0; c < list.size(); c++)
	{
		if (list[c] == server)
		{
			return;
		}
	}
	list.push_back(server);
}

// returns a list of DIRECT servernames for a specific channel
void GetListOfServersForChannel(chanrec* c, std::deque<TreeServer*> &list)
{
	CUList *ulist = c->GetUsers();
	for (CUList::iterator i = ulist->begin(); i != ulist->end(); i++)
	{
		if (i->second->GetFd() < 0)
		{
			TreeServer* best = BestRouteTo(i->second->server);
			if (best)
				AddThisServer(best,list);
		}
	}
	return;
}

bool DoOneToAllButSenderRaw(std::string data, std::string omit, std::string prefix, irc::string command, std::deque<std::string> &params)
{
	TreeServer* omitroute = BestRouteTo(omit);
	if ((command == "NOTICE") || (command == "PRIVMSG"))
	{
		if (params.size() >= 2)
		{
			/* Prefixes */
			if ((*(params[0].c_str()) == '@') || (*(params[0].c_str()) == '%') || (*(params[0].c_str()) == '+'))
			{
				params[0] = params[0].substr(1, params[0].length()-1);
			}
			if ((*(params[0].c_str()) != '#') && (*(params[0].c_str()) != '$'))
			{
				// special routing for private messages/notices
				userrec* d = ServerInstance->FindNick(params[0]);
				if (d)
				{
					std::deque<std::string> par;
					par.push_back(params[0]);
					par.push_back(":"+params[1]);
					DoOneToOne(prefix,command.c_str(),par,d->server);
					return true;
				}
			}
			else if (*(params[0].c_str()) == '$')
			{
				std::deque<std::string> par;
				par.push_back(params[0]);
				par.push_back(":"+params[1]);
				DoOneToAllButSender(prefix,command.c_str(),par,omitroute->GetName());
				return true;
			}
			else
			{
				ServerInstance->Log(DEBUG,"Channel privmsg going to chan %s",params[0].c_str());
				chanrec* c = ServerInstance->FindChan(params[0]);
				if (c)
				{
					std::deque<TreeServer*> list;
					GetListOfServersForChannel(c,list);
					ServerInstance->Log(DEBUG,"Got a list of %d servers",list.size());
					unsigned int lsize = list.size();
					for (unsigned int i = 0; i < lsize; i++)
					{
						TreeSocket* Sock = list[i]->GetSocket();
						if ((Sock) && (list[i]->GetName() != omit) && (omitroute != list[i]))
						{
							ServerInstance->Log(DEBUG,"Writing privmsg to server %s",list[i]->GetName().c_str());
							Sock->WriteLine(data);
						}
					}
					return true;
				}
			}
		}
	}
	unsigned int items = TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = TreeRoot->GetChild(x);
		if ((Route->GetSocket()) && (Route->GetName() != omit) && (omitroute != Route))
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(data);
		}
	}
	return true;
}

bool DoOneToAllButSender(std::string prefix, std::string command, std::deque<std::string> &params, std::string omit)
{
	TreeServer* omitroute = BestRouteTo(omit);
	std::string FullLine = ":" + prefix + " " + command;
	unsigned int words = params.size();
	for (unsigned int x = 0; x < words; x++)
	{
		FullLine = FullLine + " " + params[x];
	}
	unsigned int items = TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = TreeRoot->GetChild(x);
		// Send the line IF:
		// The route has a socket (its a direct connection)
		// The route isnt the one to be omitted
		// The route isnt the path to the one to be omitted
		if ((Route->GetSocket()) && (Route->GetName() != omit) && (omitroute != Route))
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(FullLine);
		}
	}
	return true;
}

bool DoOneToMany(std::string prefix, std::string command, std::deque<std::string> &params)
{
	std::string FullLine = ":" + prefix + " " + command;
	unsigned int words = params.size();
	for (unsigned int x = 0; x < words; x++)
	{
		FullLine = FullLine + " " + params[x];
	}
	unsigned int items = TreeRoot->ChildCount();
	for (unsigned int x = 0; x < items; x++)
	{
		TreeServer* Route = TreeRoot->GetChild(x);
		if (Route->GetSocket())
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(FullLine);
		}
	}
	return true;
}

bool DoOneToOne(std::string prefix, std::string command, std::deque<std::string> &params, std::string target)
{
	TreeServer* Route = BestRouteTo(target);
	if (Route)
	{
		std::string FullLine = ":" + prefix + " " + command;
		unsigned int words = params.size();
		for (unsigned int x = 0; x < words; x++)
		{
			FullLine = FullLine + " " + params[x];
		}
		if (Route->GetSocket())
		{
			TreeSocket* Sock = Route->GetSocket();
			Sock->WriteLine(FullLine);
		}
		return true;
	}
	else
	{
		return true;
	}
}

std::vector<TreeSocket*> Bindings;

void ReadConfiguration(bool rebind)
{
	Conf = new ConfigReader(ServerInstance);
	if (rebind)
	{
		for (int j =0; j < Conf->Enumerate("bind"); j++)
		{
			std::string Type = Conf->ReadValue("bind","type",j);
			std::string IP = Conf->ReadValue("bind","address",j);
			long Port = Conf->ReadInteger("bind","port",j,true);
			if (Type == "servers")
			{
				if (IP == "*")
				{
					IP = "";
				}
				TreeSocket* listener = new TreeSocket(ServerInstance, IP.c_str(),Port,true,10);
				if (listener->GetState() == I_LISTENING)
				{
					ServerInstance->AddSocket(listener);
					Bindings.push_back(listener);
				}
				else
				{
					ServerInstance->Log(DEFAULT,"m_spanningtree: Warning: Failed to bind server port %d",Port);
					listener->Close();
					DELETE(listener);
				}
			}
		}
	}
	FlatLinks = Conf->ReadFlag("options","flatlinks",0);
	HideULines = Conf->ReadFlag("options","hideulines",0);
	LinkBlocks.clear();
	ValidIPs.clear();
	for (int j =0; j < Conf->Enumerate("link"); j++)
	{
		Link L;
		std::string Allow = Conf->ReadValue("link","allowmask",j);
		L.Name = (Conf->ReadValue("link","name",j)).c_str();
		L.IPAddr = Conf->ReadValue("link","ipaddr",j);
		L.Port = Conf->ReadInteger("link","port",j,true);
		L.SendPass = Conf->ReadValue("link","sendpass",j);
		L.RecvPass = Conf->ReadValue("link","recvpass",j);
		L.AutoConnect = Conf->ReadInteger("link","autoconnect",j,true);
		L.EncryptionKey =  Conf->ReadValue("link","encryptionkey",j);
		L.HiddenFromStats = Conf->ReadFlag("link","hidden",j);
		L.NextConnectTime = time(NULL) + L.AutoConnect;
		/* Bugfix by brain, do not allow people to enter bad configurations */
		if ((L.IPAddr != "") && (L.RecvPass != "") && (L.SendPass != "") && (L.Name != "") && (L.Port))
		{
			ValidIPs.push_back(L.IPAddr);

			if (Allow.length())
				ValidIPs.push_back(Allow);

			/* Needs resolving */
			insp_inaddr binip;
			if (insp_aton(L.IPAddr.c_str(), &binip) < 1)
			{
				try
				{
					SecurityIPResolver* sr = new SecurityIPResolver(ServerInstance, L.IPAddr, L);
					ServerInstance->AddResolver(sr);
				}
				catch (ModuleException& e)
				{
					ServerInstance->Log(DEBUG,"Error in resolver: %s",e.GetReason());
				}
			}

			LinkBlocks.push_back(L);
			ServerInstance->Log(DEBUG,"m_spanningtree: Read server %s with host %s:%d",L.Name.c_str(),L.IPAddr.c_str(),L.Port);
		}
		else
		{
			if (L.IPAddr == "")
			{
				ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', IP address not defined!",L.Name.c_str());
			}
			else if (L.RecvPass == "")
			{
				ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', recvpass not defined!",L.Name.c_str());
			}
			else if (L.SendPass == "")
			{
				ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', sendpass not defined!",L.Name.c_str());
			}
			else if (L.Name == "")
			{
				ServerInstance->Log(DEFAULT,"Invalid configuration, link tag without a name!");
			}
			else if (!L.Port)
			{
				ServerInstance->Log(DEFAULT,"Invalid configuration for server '%s', no port specified!",L.Name.c_str());
			}
		}
	}
	DELETE(Conf);
}


class ModuleSpanningTree : public Module
{
	std::vector<TreeSocket*> Bindings;
	int line;
	int NumServers;
	unsigned int max_local;
	unsigned int max_global;
	cmd_rconnect* command_rconnect;

 public:

	ModuleSpanningTree(InspIRCd* Me)
		: Module::Module(Me), max_local(0), max_global(0)
	{
		
		Bindings.clear();

		::ServerInstance = Me;

		// Create the root of the tree
		TreeRoot = new TreeServer(ServerInstance, ServerInstance->Config->ServerName, ServerInstance->Config->ServerDesc);

		ReadConfiguration(true);

		command_rconnect = new cmd_rconnect(ServerInstance, this);
		ServerInstance->AddCommand(command_rconnect);
	}

	void ShowLinks(TreeServer* Current, userrec* user, int hops)
	{
		std::string Parent = TreeRoot->GetName();
		if (Current->GetParent())
		{
			Parent = Current->GetParent()->GetName();
		}
		for (unsigned int q = 0; q < Current->ChildCount(); q++)
		{
			if ((HideULines) && (ServerInstance->ULine(Current->GetChild(q)->GetName().c_str())))
			{
				if (*user->oper)
				{
					 ShowLinks(Current->GetChild(q),user,hops+1);
				}
			}
			else
			{
				ShowLinks(Current->GetChild(q),user,hops+1);
			}
		}
		/* Don't display the line if its a uline, hide ulines is on, and the user isnt an oper */
		if ((HideULines) && (ServerInstance->ULine(Current->GetName().c_str())) && (!*user->oper))
			return;
		user->WriteServ("364 %s %s %s :%d %s",user->nick,Current->GetName().c_str(),(FlatLinks && (!*user->oper)) ? ServerInstance->Config->ServerName : Parent.c_str(),(FlatLinks && (!*user->oper)) ? 0 : hops,Current->GetDesc().c_str());
	}

	int CountLocalServs()
	{
		return TreeRoot->ChildCount();
	}

	int CountServs()
	{
		return serverlist.size();
	}

	void HandleLinks(const char** parameters, int pcnt, userrec* user)
	{
		ShowLinks(TreeRoot,user,0);
		user->WriteServ("365 %s * :End of /LINKS list.",user->nick);
		return;
	}

	void HandleLusers(const char** parameters, int pcnt, userrec* user)
	{
		unsigned int n_users = ServerInstance->UserCount();

		/* Only update these when someone wants to see them, more efficient */
		if ((unsigned int)ServerInstance->LocalUserCount() > max_local)
			max_local = ServerInstance->LocalUserCount();
		if (n_users > max_global)
			max_global = n_users;

		user->WriteServ("251 %s :There are %d users and %d invisible on %d servers",user->nick,n_users-ServerInstance->InvisibleUserCount(),ServerInstance->InvisibleUserCount(),this->CountServs());
		if (ServerInstance->OperCount())
			user->WriteServ("252 %s %d :operator(s) online",user->nick,ServerInstance->OperCount());
		if (ServerInstance->UnregisteredUserCount())
			user->WriteServ("253 %s %d :unknown connections",user->nick,ServerInstance->UnregisteredUserCount());
		if (ServerInstance->ChannelCount())
			user->WriteServ("254 %s %d :channels formed",user->nick,ServerInstance->ChannelCount());
		user->WriteServ("254 %s :I have %d clients and %d servers",user->nick,ServerInstance->LocalUserCount(),this->CountLocalServs());
		user->WriteServ("265 %s :Current Local Users: %d  Max: %d",user->nick,ServerInstance->LocalUserCount(),max_local);
		user->WriteServ("266 %s :Current Global Users: %d  Max: %d",user->nick,n_users,max_global);
		return;
	}

	// WARNING: NOT THREAD SAFE - DONT GET ANY SMART IDEAS.

	void ShowMap(TreeServer* Current, userrec* user, int depth, char matrix[128][80], float &totusers, float &totservers)
	{
		if (line < 128)
		{
			for (int t = 0; t < depth; t++)
			{
				matrix[line][t] = ' ';
			}

			// For Aligning, we need to work out exactly how deep this thing is, and produce
			// a 'Spacer' String to compensate.
			char spacer[40];

			memset(spacer,' ',40);
			if ((40 - Current->GetName().length() - depth) > 1) {
				spacer[40 - Current->GetName().length() - depth] = '\0';
			}
			else
			{
				spacer[5] = '\0';
			}

			float percent;
			char text[80];
			if (ServerInstance->clientlist.size() == 0) {
				// If there are no users, WHO THE HELL DID THE /MAP?!?!?!
				percent = 0;
			}
			else
			{
				percent = ((float)Current->GetUserCount() / (float)ServerInstance->clientlist.size()) * 100;
			}
			snprintf(text, 80, "%s %s%5d [%5.2f%%]", Current->GetName().c_str(), spacer, Current->GetUserCount(), percent);
			totusers += Current->GetUserCount();
			totservers++;
			strlcpy(&matrix[line][depth],text,80);
			line++;
			for (unsigned int q = 0; q < Current->ChildCount(); q++)
			{
				if ((HideULines) && (ServerInstance->ULine(Current->GetChild(q)->GetName().c_str())))
				{
					if (*user->oper)
					{
						ShowMap(Current->GetChild(q),user,(FlatLinks && (!*user->oper)) ? depth : depth+2,matrix,totusers,totservers);
					}
				}
				else
				{
					ShowMap(Current->GetChild(q),user,(FlatLinks && (!*user->oper)) ? depth : depth+2,matrix,totusers,totservers);
				}
			}
		}
	}

	int HandleStats(const char** parameters, int pcnt, userrec* user)
	{
		if (pcnt > 1)
		{
			/* Remote STATS, the server is within the 2nd parameter */
			std::deque<std::string> params;
			params.push_back(parameters[0]);
			params.push_back(parameters[1]);
			/* Send it out remotely, generate no reply yet */
			TreeServer* s = FindServerMask(parameters[1]);
			if (s)
			{
				params[1] = s->GetName();
				DoOneToOne(user->nick, "STATS", params, s->GetName());
			}
			else
			{
				user->WriteServ( "402 %s %s :No such server", user->nick, parameters[0]);
			}
			return 1;
		}
		return 0;
	}

	// Ok, prepare to be confused.
	// After much mulling over how to approach this, it struck me that
	// the 'usual' way of doing a /MAP isnt the best way. Instead of
	// keeping track of a ton of ascii characters, and line by line
	// under recursion working out where to place them using multiplications
	// and divisons, we instead render the map onto a backplane of characters
	// (a character matrix), then draw the branches as a series of "L" shapes
	// from the nodes. This is not only friendlier on CPU it uses less stack.

	void HandleMap(const char** parameters, int pcnt, userrec* user)
	{
		// This array represents a virtual screen which we will
		// "scratch" draw to, as the console device of an irc
		// client does not provide for a proper terminal.
		float totusers = 0;
		float totservers = 0;
		char matrix[128][80];
		for (unsigned int t = 0; t < 128; t++)
		{
			matrix[t][0] = '\0';
		}
		line = 0;
		// The only recursive bit is called here.
		ShowMap(TreeRoot,user,0,matrix,totusers,totservers);
		// Process each line one by one. The algorithm has a limit of
		// 128 servers (which is far more than a spanning tree should have
		// anyway, so we're ok). This limit can be raised simply by making
		// the character matrix deeper, 128 rows taking 10k of memory.
		for (int l = 1; l < line; l++)
		{
			// scan across the line looking for the start of the
			// servername (the recursive part of the algorithm has placed
			// the servers at indented positions depending on what they
			// are related to)
			int first_nonspace = 0;
			while (matrix[l][first_nonspace] == ' ')
			{
				first_nonspace++;
			}
			first_nonspace--;
			// Draw the `- (corner) section: this may be overwritten by
			// another L shape passing along the same vertical pane, becoming
			// a |- (branch) section instead.
			matrix[l][first_nonspace] = '-';
			matrix[l][first_nonspace-1] = '`';
			int l2 = l - 1;
			// Draw upwards until we hit the parent server, causing possibly
			// other corners (`-) to become branches (|-)
			while ((matrix[l2][first_nonspace-1] == ' ') || (matrix[l2][first_nonspace-1] == '`'))
			{
				matrix[l2][first_nonspace-1] = '|';
				l2--;
			}
		}
		// dump the whole lot to the user. This is the easy bit, honest.
		for (int t = 0; t < line; t++)
		{
			user->WriteServ("006 %s :%s",user->nick,&matrix[t][0]);
		}
		float avg_users = totusers / totservers;
		user->WriteServ("270 %s :%.0f server%s and %.0f user%s, average %.2f users per server",user->nick,totservers,(totservers > 1 ? "s" : ""),totusers,(totusers > 1 ? "s" : ""),avg_users);
	user->WriteServ("007 %s :End of /MAP",user->nick);
		return;
	}

	int HandleSquit(const char** parameters, int pcnt, userrec* user)
	{
		TreeServer* s = FindServerMask(parameters[0]);
		if (s)
		{
			if (s == TreeRoot)
			{
				 user->WriteServ("NOTICE %s :*** SQUIT: Foolish mortal, you cannot make a server SQUIT itself! (%s matches local server name)",user->nick,parameters[0]);
				return 1;
			}
			TreeSocket* sock = s->GetSocket();
			if (sock)
			{
				ServerInstance->Log(DEBUG,"Splitting server %s",s->GetName().c_str());
				ServerInstance->WriteOpers("*** SQUIT: Server \002%s\002 removed from network by %s",parameters[0],user->nick);
				sock->Squit(s,"Server quit by "+std::string(user->nick)+"!"+std::string(user->ident)+"@"+std::string(user->host));
				ServerInstance->RemoveSocket(sock);
			}
			else
			{
				user->WriteServ("NOTICE %s :*** SQUIT: The server \002%s\002 is not directly connected.",user->nick,parameters[0]);
			}
		}
		else
		{
			 user->WriteServ("NOTICE %s :*** SQUIT: The server \002%s\002 does not exist on the network.",user->nick,parameters[0]);
		}
		return 1;
	}

	int HandleTime(const char** parameters, int pcnt, userrec* user)
	{
		if ((IS_LOCAL(user)) && (pcnt))
		{
			TreeServer* found = FindServerMask(parameters[0]);
			if (found)
			{
				// we dont' override for local server
				if (found == TreeRoot)
					return 0;
				
				std::deque<std::string> params;
				params.push_back(found->GetName());
				params.push_back(user->nick);
				DoOneToOne(ServerInstance->Config->ServerName,"TIME",params,found->GetName());
			}
			else
			{
				user->WriteServ("402 %s %s :No such server",user->nick,parameters[0]);
			}
		}
		return 1;
	}

	int HandleRemoteWhois(const char** parameters, int pcnt, userrec* user)
	{
		if ((IS_LOCAL(user)) && (pcnt > 1))
		{
			userrec* remote = ServerInstance->FindNick(parameters[1]);
			if ((remote) && (remote->GetFd() < 0))
			{
				std::deque<std::string> params;
				params.push_back(parameters[1]);
				DoOneToOne(user->nick,"IDLE",params,remote->server);
				return 1;
			}
			else if (!remote)
			{
				user->WriteServ("401 %s %s :No such nick/channel",user->nick, parameters[1]);
				user->WriteServ("318 %s %s :End of /WHOIS list.",user->nick, parameters[1]);
				return 1;
			}
		}
		return 0;
	}

	void DoPingChecks(time_t curtime)
	{
		for (unsigned int j = 0; j < TreeRoot->ChildCount(); j++)
		{
			TreeServer* serv = TreeRoot->GetChild(j);
			TreeSocket* sock = serv->GetSocket();
			if (sock)
			{
				if (curtime >= serv->NextPingTime())
				{
					if (serv->AnsweredLastPing())
					{
						sock->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" PING "+serv->GetName());
						serv->SetNextPingTime(curtime + 120);
					}
					else
					{
						// they didnt answer, boot them
						ServerInstance->WriteOpers("*** Server \002%s\002 pinged out",serv->GetName().c_str());
						sock->Squit(serv,"Ping timeout");
						ServerInstance->RemoveSocket(sock);
						return;
					}
				}
			}
		}
	}

	void AutoConnectServers(time_t curtime)
	{
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if ((x->AutoConnect) && (curtime >= x->NextConnectTime))
			{
				ServerInstance->Log(DEBUG,"Auto-Connecting %s",x->Name.c_str());
				x->NextConnectTime = curtime + x->AutoConnect;
				TreeServer* CheckDupe = FindServer(x->Name.c_str());
				if (!CheckDupe)
				{
					// an autoconnected server is not connected. Check if its time to connect it
					ServerInstance->WriteOpers("*** AUTOCONNECT: Auto-connecting server \002%s\002 (%lu seconds until next attempt)",x->Name.c_str(),x->AutoConnect);

					insp_inaddr binip;

					/* Do we already have an IP? If so, no need to resolve it. */
					if (insp_aton(x->IPAddr.c_str(), &binip) > 0)
					{
						TreeSocket* newsocket = new TreeSocket(ServerInstance, x->IPAddr,x->Port,false,10,x->Name.c_str());
						if (newsocket->GetFd() > -1)
						{
							ServerInstance->AddSocket(newsocket);
						}
						else
						{
							ServerInstance->WriteOpers("*** AUTOCONNECT: Error autoconnecting \002%s\002: %s.",x->Name.c_str(),strerror(errno));
							delete newsocket;
						}
					}
					else
					{
						try
						{
							ServernameResolver* snr = new ServernameResolver(ServerInstance,x->IPAddr, *x);
							ServerInstance->AddResolver(snr);
						}
						catch (ModuleException& e)
						{
							ServerInstance->Log(DEBUG,"Error in resolver: %s",e.GetReason());
						}
					}

				}
			}
		}
	}

	int HandleVersion(const char** parameters, int pcnt, userrec* user)
	{
		// we've already checked if pcnt > 0, so this is safe
		TreeServer* found = FindServerMask(parameters[0]);
		if (found)
		{
			std::string Version = found->GetVersion();
			user->WriteServ("351 %s :%s",user->nick,Version.c_str());
			if (found == TreeRoot)
			{
				std::stringstream out(ServerInstance->Config->data005);
				std::string token = "";
				std::string line5 = "";
				int token_counter = 0;

				while (!out.eof())
				{
					out >> token;
					line5 = line5 + token + " ";   
					token_counter++;

					if ((token_counter >= 13) || (out.eof() == true))
					{
						user->WriteServ("005 %s %s:are supported by this server",user->nick,line5.c_str());
						line5 = "";
						token_counter = 0;
					}
				}
			}
		}
		else
		{
			user->WriteServ("402 %s %s :No such server",user->nick,parameters[0]);
		}
		return 1;
	}
	
	int HandleConnect(const char** parameters, int pcnt, userrec* user)
	{
		for (std::vector<Link>::iterator x = LinkBlocks.begin(); x < LinkBlocks.end(); x++)
		{
			if (ServerInstance->MatchText(x->Name.c_str(),parameters[0]))
			{
				TreeServer* CheckDupe = FindServer(x->Name.c_str());
				if (!CheckDupe)
				{
					user->WriteServ("NOTICE %s :*** CONNECT: Connecting to server: \002%s\002 (%s:%d)",user->nick,x->Name.c_str(),(x->HiddenFromStats ? "<hidden>" : x->IPAddr.c_str()),x->Port);
					insp_inaddr binip;

					/* Do we already have an IP? If so, no need to resolve it. */
					if (insp_aton(x->IPAddr.c_str(), &binip) > 0)
					{
						TreeSocket* newsocket = new TreeSocket(ServerInstance,x->IPAddr,x->Port,false,10,x->Name.c_str());
						if (newsocket->GetFd() > -1)
						{
							ServerInstance->AddSocket(newsocket);
						}
						else
						{
							ServerInstance->WriteOpers("*** CONNECT: Error connecting \002%s\002: %s.",x->Name.c_str(),strerror(errno));
							delete newsocket;
						}
					}
					else
					{
						try
						{
							ServernameResolver* snr = new ServernameResolver(ServerInstance, x->IPAddr, *x);
							ServerInstance->AddResolver(snr);
						}
						catch (ModuleException& e)
						{
							ServerInstance->Log(DEBUG,"Error in resolver: %s",e.GetReason());
						}
					}
					return 1;
				}
				else
				{
					user->WriteServ("NOTICE %s :*** CONNECT: Server \002%s\002 already exists on the network and is connected via \002%s\002",user->nick,x->Name.c_str(),CheckDupe->GetParent()->GetName().c_str());
					return 1;
				}
			}
		}
		user->WriteServ("NOTICE %s :*** CONNECT: No server matching \002%s\002 could be found in the config file.",user->nick,parameters[0]);
		return 1;
	}

	virtual int OnStats(char statschar, userrec* user, string_list &results)
	{
		if (statschar == 'c')
		{
			for (unsigned int i = 0; i < LinkBlocks.size(); i++)
			{
				results.push_back(std::string(ServerInstance->Config->ServerName)+" 213 "+user->nick+" C *@"+(LinkBlocks[i].HiddenFromStats ? "<hidden>" : LinkBlocks[i].IPAddr)+" * "+LinkBlocks[i].Name.c_str()+" "+ConvToStr(LinkBlocks[i].Port)+" "+(LinkBlocks[i].EncryptionKey != "" ? 'e' : '-')+(LinkBlocks[i].AutoConnect ? 'a' : '-')+'s');
				results.push_back(std::string(ServerInstance->Config->ServerName)+" 244 "+user->nick+" H * * "+LinkBlocks[i].Name.c_str());
			}
			results.push_back(std::string(ServerInstance->Config->ServerName)+" 219 "+user->nick+" "+statschar+" :End of /STATS report");
			ServerInstance->WriteOpers("*** Notice: %s '%c' requested by %s (%s@%s)",(!strcmp(user->server,ServerInstance->Config->ServerName) ? "Stats" : "Remote stats"),statschar,user->nick,user->ident,user->host);
			return 1;
		}
		return 0;
	}

	virtual int OnPreCommand(const std::string &command, const char** parameters, int pcnt, userrec *user, bool validated)
	{
		/* If the command doesnt appear to be valid, we dont want to mess with it. */
		if (!validated)
			return 0;

		if (command == "CONNECT")
		{
			return this->HandleConnect(parameters,pcnt,user);
		}
		else if (command == "STATS")
		{
			return this->HandleStats(parameters,pcnt,user);
		}
		else if (command == "SQUIT")
		{
			return this->HandleSquit(parameters,pcnt,user);
		}
		else if (command == "MAP")
		{
			this->HandleMap(parameters,pcnt,user);
			return 1;
		}
		else if ((command == "TIME") && (pcnt > 0))
		{
			return this->HandleTime(parameters,pcnt,user);
		}
		else if (command == "LUSERS")
		{
			this->HandleLusers(parameters,pcnt,user);
			return 1;
		}
		else if (command == "LINKS")
		{
			this->HandleLinks(parameters,pcnt,user);
			return 1;
		}
		else if (command == "WHOIS")
		{
			if (pcnt > 1)
			{
				// remote whois
				return this->HandleRemoteWhois(parameters,pcnt,user);
			}
		}
		else if ((command == "VERSION") && (pcnt > 0))
		{
			this->HandleVersion(parameters,pcnt,user);
			return 1;
		}
		else if (ServerInstance->IsValidModuleCommand(command, pcnt, user))
		{
			// this bit of code cleverly routes all module commands
			// to all remote severs *automatically* so that modules
			// can just handle commands locally, without having
			// to have any special provision in place for remote
			// commands and linking protocols.
			std::deque<std::string> params;
			params.clear();
			for (int j = 0; j < pcnt; j++)
			{
				if (strchr(parameters[j],' '))
				{
					params.push_back(":" + std::string(parameters[j]));
				}
				else
				{
					params.push_back(std::string(parameters[j]));
				}
			}
			ServerInstance->Log(DEBUG,"Globally route '%s'",command.c_str());
			DoOneToMany(user->nick,command,params);
		}
		return 0;
	}

	virtual void OnGetServerDescription(const std::string &servername,std::string &description)
	{
		TreeServer* s = FindServer(servername);
		if (s)
		{
			description = s->GetDesc();
		}
	}

	virtual void OnUserInvite(userrec* source,userrec* dest,chanrec* channel)
	{
		if (IS_LOCAL(source))
		{
			std::deque<std::string> params;
			params.push_back(dest->nick);
			params.push_back(channel->name);
			DoOneToMany(source->nick,"INVITE",params);
		}
	}

	virtual void OnPostLocalTopicChange(userrec* user, chanrec* chan, const std::string &topic)
	{
		std::deque<std::string> params;
		params.push_back(chan->name);
		params.push_back(":"+topic);
		DoOneToMany(user->nick,"TOPIC",params);
	}

	virtual void OnWallops(userrec* user, const std::string &text)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(":"+text);
			DoOneToMany(user->nick,"WALLOPS",params);
		}
	}

	virtual void OnUserNotice(userrec* user, void* dest, int target_type, const std::string &text, char status)
	{
		if (target_type == TYPE_USER)
		{
			userrec* d = (userrec*)dest;
			if ((d->GetFd() < 0) && (IS_LOCAL(user)))
			{
				std::deque<std::string> params;
				params.clear();
				params.push_back(d->nick);
				params.push_back(":"+text);
				DoOneToOne(user->nick,"NOTICE",params,d->server);
			}
		}
		else if (target_type == TYPE_CHANNEL)
		{
			if (IS_LOCAL(user))
			{
				chanrec *c = (chanrec*)dest;
				std::string cname = c->name;
				if (status)
					cname = status + cname;
				std::deque<TreeServer*> list;
				GetListOfServersForChannel(c,list);
				unsigned int ucount = list.size();
				for (unsigned int i = 0; i < ucount; i++)
				{
					TreeSocket* Sock = list[i]->GetSocket();
					if (Sock)
						Sock->WriteLine(":"+std::string(user->nick)+" NOTICE "+cname+" :"+text);
				}
			}
		}
                else if (target_type == TYPE_SERVER)
		{
			if (IS_LOCAL(user))
			{
				char* target = (char*)dest;
				std::deque<std::string> par;
				par.push_back(target);
				par.push_back(":"+text);
				DoOneToMany(user->nick,"NOTICE",par);
			}
		}
	}

	virtual void OnUserMessage(userrec* user, void* dest, int target_type, const std::string &text, char status)
	{
		if (target_type == TYPE_USER)
		{
			// route private messages which are targetted at clients only to the server
			// which needs to receive them
			userrec* d = (userrec*)dest;
			if ((d->GetFd() < 0) && (IS_LOCAL(user)))
			{
				std::deque<std::string> params;
				params.clear();
				params.push_back(d->nick);
				params.push_back(":"+text);
				DoOneToOne(user->nick,"PRIVMSG",params,d->server);
			}
		}
		else if (target_type == TYPE_CHANNEL)
		{
			if (IS_LOCAL(user))
			{
				chanrec *c = (chanrec*)dest;
				std::string cname = c->name;
				if (status)
					cname = status + cname;
				std::deque<TreeServer*> list;
				GetListOfServersForChannel(c,list);
				unsigned int ucount = list.size();
				for (unsigned int i = 0; i < ucount; i++)
				{
					TreeSocket* Sock = list[i]->GetSocket();
					if (Sock)
						Sock->WriteLine(":"+std::string(user->nick)+" PRIVMSG "+cname+" :"+text);
				}
			}
		}
		else if (target_type == TYPE_SERVER)
		{
			if (IS_LOCAL(user))
			{
				char* target = (char*)dest;
				std::deque<std::string> par;
				par.push_back(target);
				par.push_back(":"+text);
				DoOneToMany(user->nick,"PRIVMSG",par);
			}
		}
	}

	virtual void OnBackgroundTimer(time_t curtime)
	{
		AutoConnectServers(curtime);
		DoPingChecks(curtime);
	}

	virtual void OnUserJoin(userrec* user, chanrec* channel)
	{
		// Only do this for local users
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.clear();
			params.push_back(channel->name);

			if (channel->GetUserCounter() > 1)
			{
				// not the first in the channel
				DoOneToMany(user->nick,"JOIN",params);
			}
			else
			{
				// first in the channel, set up their permissions
				// and the channel TS with FJOIN.
				char ts[24];
				snprintf(ts,24,"%lu",(unsigned long)channel->age);
				params.clear();
				params.push_back(channel->name);
				params.push_back(ts);
				params.push_back("@,"+std::string(user->nick));
				DoOneToMany(ServerInstance->Config->ServerName,"FJOIN",params);
			}
		}
	}

	virtual void OnChangeHost(userrec* user, const std::string &newhost)
	{
		// only occurs for local clients
		if (user->registered != REG_ALL)
			return;
		std::deque<std::string> params;
		params.push_back(newhost);
		DoOneToMany(user->nick,"FHOST",params);
	}

	virtual void OnChangeName(userrec* user, const std::string &gecos)
	{
		// only occurs for local clients
		if (user->registered != REG_ALL)
			return;
		std::deque<std::string> params;
		params.push_back(gecos);
		DoOneToMany(user->nick,"FNAME",params);
	}

	virtual void OnUserPart(userrec* user, chanrec* channel, const std::string &partmessage)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(channel->name);
			if (partmessage != "")
				params.push_back(":"+partmessage);
			DoOneToMany(user->nick,"PART",params);
		}
	}

	virtual void OnUserConnect(userrec* user)
	{
		char agestr[MAXBUF];
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			snprintf(agestr,MAXBUF,"%lu",(unsigned long)user->age);
			params.push_back(agestr);
			params.push_back(user->nick);
			params.push_back(user->host);
			params.push_back(user->dhost);
			params.push_back(user->ident);
			params.push_back("+"+std::string(user->FormatModes()));
			params.push_back(user->GetIPString());
			params.push_back(":"+std::string(user->fullname));
			DoOneToMany(ServerInstance->Config->ServerName,"NICK",params);

			// User is Local, change needs to be reflected!
			TreeServer* SourceServer = FindServer(user->server);
			if (SourceServer)
			{
				SourceServer->AddUserCount();
			}

		}
	}

	virtual void OnUserQuit(userrec* user, const std::string &reason)
	{
		if ((IS_LOCAL(user)) && (user->registered == REG_ALL))
		{
			std::deque<std::string> params;
			params.push_back(":"+reason);
			DoOneToMany(user->nick,"QUIT",params);
		}
		// Regardless, We need to modify the user Counts..
		TreeServer* SourceServer = FindServer(user->server);
		if (SourceServer)
		{
			SourceServer->DelUserCount();
		}

	}

	virtual void OnUserPostNick(userrec* user, const std::string &oldnick)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(user->nick);
			DoOneToMany(oldnick,"NICK",params);
		}
	}

	virtual void OnUserKick(userrec* source, userrec* user, chanrec* chan, const std::string &reason)
	{
		if ((source) && (IS_LOCAL(source)))
		{
			std::deque<std::string> params;
			params.push_back(chan->name);
			params.push_back(user->nick);
			params.push_back(":"+reason);
			DoOneToMany(source->nick,"KICK",params);
		}
		else if (!source)
		{
			std::deque<std::string> params;
			params.push_back(chan->name);
			params.push_back(user->nick);
			params.push_back(":"+reason);
			DoOneToMany(ServerInstance->Config->ServerName,"KICK",params);
		}
	}

	virtual void OnRemoteKill(userrec* source, userrec* dest, const std::string &reason)
	{
		std::deque<std::string> params;
		params.push_back(dest->nick);
		params.push_back(":"+reason);
		DoOneToMany(source->nick,"KILL",params);
	}

	virtual void OnRehash(const std::string &parameter)
	{
		if (parameter != "")
		{
			std::deque<std::string> params;
			params.push_back(parameter);
			DoOneToMany(ServerInstance->Config->ServerName,"REHASH",params);
			// check for self
			if (ServerInstance->MatchText(ServerInstance->Config->ServerName,parameter))
			{
				ServerInstance->WriteOpers("*** Remote rehash initiated from server \002%s\002",ServerInstance->Config->ServerName);
				ServerInstance->RehashServer();
			}
		}
		ReadConfiguration(false);
	}

	// note: the protocol does not allow direct umode +o except
	// via NICK with 8 params. sending OPERTYPE infers +o modechange
	// locally.
	virtual void OnOper(userrec* user, const std::string &opertype)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(opertype);
			DoOneToMany(user->nick,"OPERTYPE",params);
		}
	}

	void OnLine(userrec* source, const std::string &host, bool adding, char linetype, long duration, const std::string &reason)
	{
		if (IS_LOCAL(source))
		{
			char type[8];
			snprintf(type,8,"%cLINE",linetype);
			std::string stype = type;
			if (adding)
			{
				char sduration[MAXBUF];
				snprintf(sduration,MAXBUF,"%ld",duration);
				std::deque<std::string> params;
				params.push_back(host);
				params.push_back(sduration);
				params.push_back(":"+reason);
				DoOneToMany(source->nick,stype,params);
			}
			else
			{
				std::deque<std::string> params;
				params.push_back(host);
				DoOneToMany(source->nick,stype,params);
			}
		}
	}

	virtual void OnAddGLine(long duration, userrec* source, const std::string &reason, const std::string &hostmask)
	{
		OnLine(source,hostmask,true,'G',duration,reason);
	}
	
	virtual void OnAddZLine(long duration, userrec* source, const std::string &reason, const std::string &ipmask)
	{
		OnLine(source,ipmask,true,'Z',duration,reason);
	}

	virtual void OnAddQLine(long duration, userrec* source, const std::string &reason, const std::string &nickmask)
	{
		OnLine(source,nickmask,true,'Q',duration,reason);
	}

	virtual void OnAddELine(long duration, userrec* source, const std::string &reason, const std::string &hostmask)
	{
		OnLine(source,hostmask,true,'E',duration,reason);
	}

	virtual void OnDelGLine(userrec* source, const std::string &hostmask)
	{
		OnLine(source,hostmask,false,'G',0,"");
	}

	virtual void OnDelZLine(userrec* source, const std::string &ipmask)
	{
		OnLine(source,ipmask,false,'Z',0,"");
	}

	virtual void OnDelQLine(userrec* source, const std::string &nickmask)
	{
		OnLine(source,nickmask,false,'Q',0,"");
	}

	virtual void OnDelELine(userrec* source, const std::string &hostmask)
	{
		OnLine(source,hostmask,false,'E',0,"");
	}

	virtual void OnMode(userrec* user, void* dest, int target_type, const std::string &text)
	{
		if ((IS_LOCAL(user)) && (user->registered == REG_ALL))
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)dest;
				std::deque<std::string> params;
				params.push_back(u->nick);
				params.push_back(text);
				DoOneToMany(user->nick,"MODE",params);
			}
			else
			{
				chanrec* c = (chanrec*)dest;
				std::deque<std::string> params;
				params.push_back(c->name);
				params.push_back(text);
				DoOneToMany(user->nick,"MODE",params);
			}
		}
	}

	virtual void OnSetAway(userrec* user)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.push_back(":"+std::string(user->awaymsg));
			DoOneToMany(user->nick,"AWAY",params);
		}
	}

	virtual void OnCancelAway(userrec* user)
	{
		if (IS_LOCAL(user))
		{
			std::deque<std::string> params;
			params.clear();
			DoOneToMany(user->nick,"AWAY",params);
		}
	}

	virtual void ProtoSendMode(void* opaque, int target_type, void* target, const std::string &modeline)
	{
		TreeSocket* s = (TreeSocket*)opaque;
		if (target)
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" FMODE "+u->nick+" "+ConvToStr(u->age)+" "+modeline);
			}
			else
			{
				chanrec* c = (chanrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" FMODE "+c->name+" "+ConvToStr(c->age)+" "+modeline);
			}
		}
	}

	virtual void ProtoSendMetaData(void* opaque, int target_type, void* target, const std::string &extname, const std::string &extdata)
	{
		TreeSocket* s = (TreeSocket*)opaque;
		if (target)
		{
			if (target_type == TYPE_USER)
			{
				userrec* u = (userrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" METADATA "+u->nick+" "+extname+" :"+extdata);
			}
			else if (target_type == TYPE_OTHER)
			{
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" METADATA * "+extname+" :"+extdata);
			}
			else if (target_type == TYPE_CHANNEL)
			{
				chanrec* c = (chanrec*)target;
				s->WriteLine(std::string(":")+ServerInstance->Config->ServerName+" METADATA "+c->name+" "+extname+" :"+extdata);
			}
		}
	}

	virtual void OnEvent(Event* event)
	{
		if (event->GetEventID() == "send_metadata")
		{
			std::deque<std::string>* params = (std::deque<std::string>*)event->GetData();
			if (params->size() < 3)
				return;
			(*params)[2] = ":" + (*params)[2];
			DoOneToMany(ServerInstance->Config->ServerName,"METADATA",*params);
		}
		else if (event->GetEventID() == "send_mode")
		{
			std::deque<std::string>* params = (std::deque<std::string>*)event->GetData();
			if (params->size() < 2)
				return;
			// Insert the TS value of the object, either userrec or chanrec
			time_t ourTS = 0;
			userrec* a = ServerInstance->FindNick((*params)[0]);
			if (a)
			{
				ourTS = a->age;
			}
			else
			{
				chanrec* a = ServerInstance->FindChan((*params)[0]);
				if (a)
				{
					ourTS = a->age;
				}
			}
			params->insert(params->begin() + 1,ConvToStr(ourTS));
			DoOneToMany(ServerInstance->Config->ServerName,"FMODE",*params);
		}
	}

	virtual ~ModuleSpanningTree()
	{
	}

	virtual Version GetVersion()
	{
		return Version(1,0,0,0,VF_STATIC|VF_VENDOR);
	}

	void Implements(char* List)
	{
		List[I_OnPreCommand] = List[I_OnGetServerDescription] = List[I_OnUserInvite] = List[I_OnPostLocalTopicChange] = 1;
		List[I_OnWallops] = List[I_OnUserNotice] = List[I_OnUserMessage] = List[I_OnBackgroundTimer] = 1;
		List[I_OnUserJoin] = List[I_OnChangeHost] = List[I_OnChangeName] = List[I_OnUserPart] = List[I_OnUserConnect] = 1;
		List[I_OnUserQuit] = List[I_OnUserPostNick] = List[I_OnUserKick] = List[I_OnRemoteKill] = List[I_OnRehash] = 1;
		List[I_OnOper] = List[I_OnAddGLine] = List[I_OnAddZLine] = List[I_OnAddQLine] = List[I_OnAddELine] = 1;
		List[I_OnDelGLine] = List[I_OnDelZLine] = List[I_OnDelQLine] = List[I_OnDelELine] = List[I_ProtoSendMode] = List[I_OnMode] = 1;
		List[I_OnStats] = List[I_ProtoSendMetaData] = List[I_OnEvent] = List[I_OnSetAway] = List[I_OnCancelAway] = 1;
	}

	/* It is IMPORTANT that m_spanningtree is the last module in the chain
	 * so that any activity it sees is FINAL, e.g. we arent going to send out
	 * a NICK message before m_cloaking has finished putting the +x on the user,
	 * etc etc.
	 * Therefore, we return PRIORITY_LAST to make sure we end up at the END of
	 * the module call queue.
	 */
	Priority Prioritize()
	{
		return PRIORITY_LAST;
	}
};


class ModuleSpanningTreeFactory : public ModuleFactory
{
 public:
	ModuleSpanningTreeFactory()
	{
	}
	
	~ModuleSpanningTreeFactory()
	{
	}
	
	virtual Module * CreateModule(InspIRCd* Me)
	{
		TreeProtocolModule = new ModuleSpanningTree(Me);
		return TreeProtocolModule;
	}
	
};


extern "C" void * init_module( void )
{
	return new ModuleSpanningTreeFactory;
}
