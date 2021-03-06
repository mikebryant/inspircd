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

#include "inspircd_config.h"
#include "inspircd.h"
#include <string>
#include <map>
#include <sstream>
#include <vector>
#include <deque>
#include "users.h"
#include "ctables.h"
#include "globals.h"
#include "modules.h"
#include "dynamic.h"
#include "wildcard.h"
#include "commands.h"
#include "xline.h"
#include "inspstring.h"

#include "hashcomp.h"
#include "typedefs.h"
#include "configreader.h"
#include "cull_list.h"

/* Version two, now with optimized expiry!
 *
 * Because the old way was horrendously slow, the new way of expiring xlines is very
 * very efficient. I have improved the efficiency of the algorithm in two ways:
 *
 * (1) There are now two lists of items for each linetype. One list holds temporary
 *     items, and the other list holds permanent items (ones which will expire).
 *     Items which are on the permanent list are NEVER checked at all by the
 *     expire_lines() function.
 * (2) The temporary xline lists are always kept in strict numerical order, keyed by 
 *     current time + duration. This means that the line which is due to expire the
 *     soonest is always pointed at by vector::begin(), so a simple while loop can
 *     very efficiently, very quickly and above all SAFELY pick off the first few
 *     items in the vector which need zapping.
 *
 *     -- Brain
 */

bool InitXLine(ServerConfig* conf, const char* tag)
{
	return true;
}

bool DoneXLine(ServerConfig* conf, const char* tag)
{
	conf->GetInstance()->XLines->apply_lines(APPLY_ALL);
	return true;
}

bool DoZLine(ServerConfig* conf, const char* tag, char** entries, void** values, int* types)
{
	char* reason = (char*)values[0];
	char* ipmask = (char*)values[1];
	
	conf->GetInstance()->XLines->add_zline(0,"<Config>",reason,ipmask);
	conf->GetInstance()->Log(DEBUG,"Read Z line (badip tag): ipmask=%s reason=%s",ipmask,reason);
	return true;
}

bool DoQLine(ServerConfig* conf, const char* tag, char** entries, void** values, int* types)
{
	char* reason = (char*)values[0];
	char* nick = (char*)values[1];
	
	conf->GetInstance()->XLines->add_qline(0,"<Config>",reason,nick);
	conf->GetInstance()->Log(DEBUG,"Read Q line (badnick tag): nick=%s reason=%s",nick,reason);
	return true;
}

bool DoKLine(ServerConfig* conf, const char* tag, char** entries, void** values, int* types)
{
	char* reason = (char*)values[0];
	char* host = (char*)values[1];
	
	conf->GetInstance()->XLines->add_kline(0,"<Config>",reason,host);
	conf->GetInstance()->Log(DEBUG,"Read K line (badhost tag): host=%s reason=%s",host,reason);
	return true;
}

bool DoELine(ServerConfig* conf, const char* tag, char** entries, void** values, int* types)
{
	char* reason = (char*)values[0];
	char* host = (char*)values[1];
	
	conf->GetInstance()->XLines->add_eline(0,"<Config>",reason,host);
	conf->GetInstance()->Log(DEBUG,"Read E line (exception tag): host=%s reason=%s",host,reason);
	return true;
}

// adds a g:line

bool XLineManager::add_gline(long duration, const char* source,const char* reason,const char* hostmask)
{
	bool ret = del_gline(hostmask);
	
	GLine item;
	item.duration = duration;
	strlcpy(item.hostmask,hostmask,199);
	strlcpy(item.reason,reason,MAXBUF);
	strlcpy(item.source,source,255);
	item.n_matches = 0;
	item.set_time = ServerInstance->Time();
	
	if (duration)
	{
		glines.push_back(item);
		sort(glines.begin(), glines.end(),XLineManager::GSortComparison);
	}
	else
	{
		pglines.push_back(item);
	}
	
	return !ret;
}

// adds an e:line (exception to bans)

bool XLineManager::add_eline(long duration, const char* source, const char* reason, const char* hostmask)
{
	bool ret = del_eline(hostmask);
	ELine item;
	item.duration = duration;
	strlcpy(item.hostmask,hostmask,199);
	strlcpy(item.reason,reason,MAXBUF);
	strlcpy(item.source,source,255);
	item.n_matches = 0;
	item.set_time = ServerInstance->Time();
	if (duration)
	{
		elines.push_back(item);
		sort(elines.begin(), elines.end(),XLineManager::ESortComparison);
	}
	else
	{
		pelines.push_back(item);
	}
	return !ret;
}

// adds a q:line

bool XLineManager::add_qline(long duration, const char* source, const char* reason, const char* nickname)
{
	bool ret = del_qline(nickname);
	QLine item;
	item.duration = duration;
	strlcpy(item.nick,nickname,63);
	strlcpy(item.reason,reason,MAXBUF);
	strlcpy(item.source,source,255);
	item.n_matches = 0;
	item.is_global = false;
	item.set_time = ServerInstance->Time();
	if (duration)
	{
		qlines.push_back(item);
		sort(qlines.begin(), qlines.end(),XLineManager::QSortComparison);
	}
	else
	{
		pqlines.push_back(item);
	}
	return !ret;
}

// adds a z:line

bool XLineManager::add_zline(long duration, const char* source, const char* reason, const char* ipaddr)
{
	bool ret = del_zline(ipaddr);
	ZLine item;
	item.duration = duration;
	if (strchr(ipaddr,'@'))
	{
		while (*ipaddr != '@')
			ipaddr++;
		ipaddr++;
	}
	strlcpy(item.ipaddr,ipaddr,39);
	strlcpy(item.reason,reason,MAXBUF);
	strlcpy(item.source,source,255);
	item.n_matches = 0;
	item.is_global = false;
	item.set_time = ServerInstance->Time();
	if (duration)
	{
		zlines.push_back(item);
		sort(zlines.begin(), zlines.end(),XLineManager::ZSortComparison);
	}
	else
	{
		pzlines.push_back(item);
	}
	return !ret;
}

// adds a k:line

bool XLineManager::add_kline(long duration, const char* source, const char* reason, const char* hostmask)
{
	bool ret = del_kline(hostmask);
	KLine item;
	item.duration = duration;
	strlcpy(item.hostmask,hostmask,200);
	strlcpy(item.reason,reason,MAXBUF);
	strlcpy(item.source,source,255);
	item.n_matches = 0;
	item.set_time = ServerInstance->Time();
	if (duration)
	{
		klines.push_back(item);
		sort(klines.begin(), klines.end(),XLineManager::KSortComparison);
	}
	else
	{
		pklines.push_back(item);
	}
	return !ret;
}

// deletes a g:line, returns true if the line existed and was removed

bool XLineManager::del_gline(const char* hostmask)
{
	for (std::vector<GLine>::iterator i = glines.begin(); i != glines.end(); i++)
	{
		if (!strcasecmp(hostmask,i->hostmask))
		{
			glines.erase(i);
			return true;
		}
	}
	for (std::vector<GLine>::iterator i = pglines.begin(); i != pglines.end(); i++)
	{
		if (!strcasecmp(hostmask,i->hostmask))
		{
			pglines.erase(i);
			return true;
		}
	}
	return false;
}

// deletes a e:line, returns true if the line existed and was removed

bool XLineManager::del_eline(const char* hostmask)
{
	for (std::vector<ELine>::iterator i = elines.begin(); i != elines.end(); i++)
	{
		if (!strcasecmp(hostmask,i->hostmask))
		{
			elines.erase(i);
			return true;
		}
	}
	for (std::vector<ELine>::iterator i = pelines.begin(); i != pelines.end(); i++)
	{
		if (!strcasecmp(hostmask,i->hostmask))
		{
			pelines.erase(i);
			return true;
		}
	}
	return false;
}

// deletes a q:line, returns true if the line existed and was removed

bool XLineManager::del_qline(const char* nickname)
{
	for (std::vector<QLine>::iterator i = qlines.begin(); i != qlines.end(); i++)
	{
		if (!strcasecmp(nickname,i->nick))
		{
			qlines.erase(i);
			return true;
		}
	}
	for (std::vector<QLine>::iterator i = pqlines.begin(); i != pqlines.end(); i++)
	{
		if (!strcasecmp(nickname,i->nick))
		{
			pqlines.erase(i);
			return true;
		}
	}
	return false;
}

bool XLineManager::qline_make_global(const char* nickname)
{
	for (std::vector<QLine>::iterator i = qlines.begin(); i != qlines.end(); i++)
	{
		if (!strcasecmp(nickname,i->nick))
		{
			i->is_global = true;
			return true;
		}
	}
	return false;
}

bool XLineManager::zline_make_global(const char* ipaddr)
{
	for (std::vector<ZLine>::iterator i = zlines.begin(); i != zlines.end(); i++)
	{
		if (!strcasecmp(ipaddr,i->ipaddr))
		{
			i->is_global = true;
			return true;
		}
	}
	return false;
}

// deletes a z:line, returns true if the line existed and was removed

bool XLineManager::del_zline(const char* ipaddr)
{
	for (std::vector<ZLine>::iterator i = zlines.begin(); i != zlines.end(); i++)
	{
		if (!strcasecmp(ipaddr,i->ipaddr))
		{
			zlines.erase(i);
			return true;
		}
	}
	for (std::vector<ZLine>::iterator i = pzlines.begin(); i != pzlines.end(); i++)
	{
		if (!strcasecmp(ipaddr,i->ipaddr))
		{
			pzlines.erase(i);
			return true;
		}
	}
	return false;
}

// deletes a k:line, returns true if the line existed and was removed

bool XLineManager::del_kline(const char* hostmask)
{
	for (std::vector<KLine>::iterator i = klines.begin(); i != klines.end(); i++)
	{
		if (!strcasecmp(hostmask,i->hostmask))
		{
			klines.erase(i);
			return true;
		}
	}
	for (std::vector<KLine>::iterator i = pklines.begin(); i != pklines.end(); i++)
	{
		if (!strcasecmp(hostmask,i->hostmask))
		{
			pklines.erase(i);
			return true;
		}
	}
	return false;
}

// returns a pointer to the reason if a nickname matches a qline, NULL if it didnt match

char* XLineManager::matches_qline(const char* nick)
{
	if ((qlines.empty()) && (pqlines.empty()))
		return NULL;
	for (std::vector<QLine>::iterator i = qlines.begin(); i != qlines.end(); i++)
		if (match(nick,i->nick))
			return i->reason;
	for (std::vector<QLine>::iterator i = pqlines.begin(); i != pqlines.end(); i++)
		if (match(nick,i->nick))
			return i->reason;
	return NULL;
}

// returns a pointer to the reason if a host matches a gline, NULL if it didnt match

char* XLineManager::matches_gline(const char* host)
{
	if ((glines.empty()) && (pglines.empty()))
		return NULL;
	for (std::vector<GLine>::iterator i = glines.begin(); i != glines.end(); i++)
		if (match(host,i->hostmask, true))
			return i->reason;
	for (std::vector<GLine>::iterator i = pglines.begin(); i != pglines.end(); i++)
		if (match(host,i->hostmask, true))
			return i->reason;
	return NULL;
}

char* XLineManager::matches_exception(const char* host)
{
	if ((elines.empty()) && (pelines.empty()))
		return NULL;
	char host2[MAXBUF];
	snprintf(host2,MAXBUF,"*@%s",host);
	for (std::vector<ELine>::iterator i = elines.begin(); i != elines.end(); i++)
		if ((match(host,i->hostmask)) || (match(host2,i->hostmask, true)))
			return i->reason;
	for (std::vector<ELine>::iterator i = pelines.begin(); i != pelines.end(); i++)
		if ((match(host,i->hostmask)) || (match(host2,i->hostmask, true)))
			return i->reason;
	return NULL;
}


void XLineManager::gline_set_creation_time(const char* host, time_t create_time)
{
	for (std::vector<GLine>::iterator i = glines.begin(); i != glines.end(); i++)
	{
		if (!strcasecmp(host,i->hostmask))
		{
			i->set_time = create_time;
			return;
		}
	}
	for (std::vector<GLine>::iterator i = pglines.begin(); i != pglines.end(); i++)
	{
		if (!strcasecmp(host,i->hostmask))
		{
			i->set_time = create_time;
			return;
		}
	}
	return ;	
}

void XLineManager::eline_set_creation_time(const char* host, time_t create_time)
{
	for (std::vector<ELine>::iterator i = elines.begin(); i != elines.end(); i++)
	{
		if (!strcasecmp(host,i->hostmask))
		{
			i->set_time = create_time;
			return;
		}
	}
	for (std::vector<ELine>::iterator i = pelines.begin(); i != pelines.end(); i++)	
	{
		if (!strcasecmp(host,i->hostmask))
		{
			i->set_time = create_time;
			return;
		}
	}
	return;
}

void XLineManager::qline_set_creation_time(const char* nick, time_t create_time)
{
	for (std::vector<QLine>::iterator i = qlines.begin(); i != qlines.end(); i++)
	{
		if (!strcasecmp(nick,i->nick))
		{
			i->set_time = create_time;
			return;
		}
	}
	for (std::vector<QLine>::iterator i = pqlines.begin(); i != pqlines.end(); i++)
	{
		if (!strcasecmp(nick,i->nick))
		{
			i->set_time = create_time;
			return;
		}
	}
	return;
}

void XLineManager::zline_set_creation_time(const char* ip, time_t create_time)
{
	for (std::vector<ZLine>::iterator i = zlines.begin(); i != zlines.end(); i++)
	{
		if (!strcasecmp(ip,i->ipaddr))
		{
			i->set_time = create_time;
			return;
		}
	}
	for (std::vector<ZLine>::iterator i = pzlines.begin(); i != pzlines.end(); i++)
	{
		if (!strcasecmp(ip,i->ipaddr))
		{
			i->set_time = create_time;
			return;
		}
	}
	return;
}

// returns a pointer to the reason if an ip address matches a zline, NULL if it didnt match

char* XLineManager::matches_zline(const char* ipaddr)
{
	if ((zlines.empty()) && (pzlines.empty()))
		return NULL;
	for (std::vector<ZLine>::iterator i = zlines.begin(); i != zlines.end(); i++)
		if (match(ipaddr,i->ipaddr, true))
			return i->reason;
	for (std::vector<ZLine>::iterator i = pzlines.begin(); i != pzlines.end(); i++)
		if (match(ipaddr,i->ipaddr, true))
			return i->reason;
	return NULL;
}

// returns a pointer to the reason if a host matches a kline, NULL if it didnt match

char* XLineManager::matches_kline(const char* host)
{
	if ((klines.empty()) && (pklines.empty()))
		return NULL;
	for (std::vector<KLine>::iterator i = klines.begin(); i != klines.end(); i++)
		if (match(host,i->hostmask, true))
			return i->reason;
	for (std::vector<KLine>::iterator i = pklines.begin(); i != pklines.end(); i++)
		if (match(host,i->hostmask, true))
			return i->reason;
	return NULL;
}

bool XLineManager::GSortComparison ( const GLine one, const GLine two )
{
	return (one.duration + one.set_time) < (two.duration + two.set_time);
}

bool XLineManager::ESortComparison ( const ELine one, const ELine two )
{
	return (one.duration + one.set_time) < (two.duration + two.set_time);
}

bool XLineManager::ZSortComparison ( const ZLine one, const ZLine two )
{
	return (one.duration + one.set_time) < (two.duration + two.set_time);
}

bool XLineManager::KSortComparison ( const KLine one, const KLine two )
{
	return (one.duration + one.set_time) < (two.duration + two.set_time);
}

bool XLineManager::QSortComparison ( const QLine one, const QLine two )
{
	return (one.duration + one.set_time) < (two.duration + two.set_time);
}

// removes lines that have expired

void XLineManager::expire_lines()
{
	time_t current = ServerInstance->Time();

	/* Because we now store all our XLines in sorted order using (i->duration + i->set_time) as a key, this
	 * means that to expire the XLines we just need to do a while, picking off the top few until there are
	 * none left at the head of the queue that are after the current time.
	 */

	while ((glines.size()) && (current > (glines.begin()->duration + glines.begin()->set_time)))
	{
		std::vector<GLine>::iterator i = glines.begin();
		ServerInstance->WriteOpers("*** Expiring timed G-Line %s (set by %s %d seconds ago)",i->hostmask,i->source,i->duration);
		glines.erase(i);
	}

	while ((elines.size()) && (current > (elines.begin()->duration + elines.begin()->set_time)))
	{
		std::vector<ELine>::iterator i = elines.begin();
		ServerInstance->WriteOpers("*** Expiring timed E-Line %s (set by %s %d seconds ago)",i->hostmask,i->source,i->duration);
		elines.erase(i);
	}

	while ((zlines.size()) && (current > (zlines.begin()->duration + zlines.begin()->set_time)))
	{
		std::vector<ZLine>::iterator i = zlines.begin();
		ServerInstance->WriteOpers("*** Expiring timed Z-Line %s (set by %s %d seconds ago)",i->ipaddr,i->source,i->duration);
		zlines.erase(i);
	}

	while ((klines.size()) && (current > (klines.begin()->duration + klines.begin()->set_time)))
	{
		std::vector<KLine>::iterator i = klines.begin();
		ServerInstance->WriteOpers("*** Expiring timed K-Line %s (set by %s %d seconds ago)",i->hostmask,i->source,i->duration);
		klines.erase(i);
	}

	while ((qlines.size()) && (current > (qlines.begin()->duration + qlines.begin()->set_time)))
	{
		std::vector<QLine>::iterator i = qlines.begin();
		ServerInstance->WriteOpers("*** Expiring timed Q-Line %s (set by %s %d seconds ago)",i->nick,i->source,i->duration);
		qlines.erase(i);
	}
	
}

// applies lines, removing clients and changing nicks etc as applicable

void XLineManager::apply_lines(const int What)
{
	char reason[MAXBUF];
	char host[MAXBUF];

	if ((!glines.size()) && (!klines.size()) && (!zlines.size()) && (!qlines.size()) &&
	(!pglines.size()) && (!pklines.size()) && (!pzlines.size()) && (!pqlines.size()))
		return;

	CullList* Goners = new CullList(ServerInstance);
	char* check = NULL;
	for (std::vector<userrec*>::const_iterator u2 = ServerInstance->local_users.begin(); u2 != ServerInstance->local_users.end(); u2++)
	{
		userrec* u = (userrec*)(*u2);
		u->MakeHost(host);
		if (elines.size() || pelines.size())
		{
			// ignore people matching exempts
			if (matches_exception(host))
				continue;
		}
		if ((What & APPLY_GLINES) && (glines.size() || pglines.size()))
		{
			if ((check = matches_gline(host)))
			{
				snprintf(reason,MAXBUF,"G-Lined: %s",check);
				Goners->AddItem(u,reason);
			}
		}
		if ((What & APPLY_KLINES) && (klines.size() || pklines.size()))
		{
			if ((check = matches_kline(host)))
			{
				snprintf(reason,MAXBUF,"K-Lined: %s",check);
				Goners->AddItem(u,reason);
			}
		}
		if ((What & APPLY_QLINES) && (qlines.size() || pqlines.size()))
		{
			if ((check = matches_qline(u->nick)))
			{
				snprintf(reason,MAXBUF,"Q-Lined: %s",check);
				Goners->AddItem(u,reason);
			}
		}
		if ((What & APPLY_ZLINES) && (zlines.size() || pzlines.size()))
		{
			if ((check = matches_zline(u->GetIPString())))
			{
				snprintf(reason,MAXBUF,"Z-Lined: %s",check);
				Goners->AddItem(u,reason);
			}
		}
	}

	Goners->Apply();
	DELETE(Goners);
}

void XLineManager::stats_k(userrec* user, string_list &results)
{
	std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<KLine>::iterator i = klines.begin(); i != klines.end(); i++)
		results.push_back(sn+" 216 "+user->nick+" :"+i->hostmask+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
	for (std::vector<KLine>::iterator i = pklines.begin(); i != pklines.end(); i++)
		results.push_back(sn+" 216 "+user->nick+" :"+i->hostmask+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
}

void XLineManager::stats_g(userrec* user, string_list &results)
{
	std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<GLine>::iterator i = glines.begin(); i != glines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+i->hostmask+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
	for (std::vector<GLine>::iterator i = pglines.begin(); i != pglines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+i->hostmask+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
}

void XLineManager::stats_q(userrec* user, string_list &results)
{
	std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<QLine>::iterator i = qlines.begin(); i != qlines.end(); i++)
		results.push_back(sn+" 217 "+user->nick+" :"+i->nick+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
	for (std::vector<QLine>::iterator i = pqlines.begin(); i != pqlines.end(); i++)
		results.push_back(sn+" 217 "+user->nick+" :"+i->nick+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
}

void XLineManager::stats_z(userrec* user, string_list &results)
{
	std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<ZLine>::iterator i = zlines.begin(); i != zlines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+i->ipaddr+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
	for (std::vector<ZLine>::iterator i = pzlines.begin(); i != pzlines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+i->ipaddr+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
}

void XLineManager::stats_e(userrec* user, string_list &results)
{
	std::string sn = ServerInstance->Config->ServerName;
	for (std::vector<ELine>::iterator i = elines.begin(); i != elines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+i->hostmask+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
	for (std::vector<ELine>::iterator i = pelines.begin(); i != pelines.end(); i++)
		results.push_back(sn+" 223 "+user->nick+" :"+i->hostmask+" "+ConvToStr(i->set_time)+" "+ConvToStr(i->duration)+" "+i->source+" :"+i->reason);
}

XLineManager::XLineManager(InspIRCd* Instance) : ServerInstance(Instance)
{
}
