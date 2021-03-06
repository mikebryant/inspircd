#ifndef __TYPEDEF_H__
#define __TYPEDEF_H__

#include <string>
#include "inspircd_config.h"
#include "hash_map.h"
#include "users.h"
#include "channels.h"
#include "hashcomp.h"
#include "inspstring.h"
#include "ctables.h"
#include "modules.h"
#include "globals.h"

typedef nspace::hash_map<std::string, userrec*, nspace::hash<string>, irc::StrHashComp> user_hash;
typedef nspace::hash_map<std::string, chanrec*, nspace::hash<string>, irc::StrHashComp> chan_hash;

typedef std::vector<std::string> servernamelist;
typedef std::deque<std::string> file_cache;

#endif
