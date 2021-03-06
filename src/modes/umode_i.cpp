#include "inspircd.h"
#include "mode.h"
#include "channels.h"
#include "users.h"
#include "modes/umode_i.h"

ModeUserInvisible::ModeUserInvisible(InspIRCd* Instance) : ModeHandler(Instance, 'i', 0, 0, false, MODETYPE_USER, false)
{
}

ModeAction ModeUserInvisible::OnModeChange(userrec* source, userrec* dest, chanrec* channel, std::string &parameter, bool adding)
{
	/* Only opers can change other users modes */
	if ((source != dest) && (!*source->oper))
		return MODEACTION_DENY;

	/* Set the bitfields */
	if (dest->modes[UM_INVISIBLE] != adding)
	{
		dest->modes[UM_INVISIBLE] = adding;
		return MODEACTION_ALLOW;
	}

	/* Allow the change */
	return MODEACTION_DENY;
}
