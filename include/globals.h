/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *                <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#ifndef __WORLD_H
#define __WORLD_H

#include <string>
#include <deque>
#include <map>
#include <vector>

typedef std::deque<std::string> file_cache;
typedef std::pair< std::string, std::string > KeyVal;
typedef std::vector< KeyVal > KeyValList;
typedef std::multimap< std::string, KeyValList > ConfigDataHash;

#endif
