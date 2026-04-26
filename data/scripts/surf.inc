EventScript_UseSurf::
	goto_if_questlog EventScript_UseSurfEnd
	checkpartymove MOVE_SURF
	goto_if_ne VAR_RESULT, PARTY_SIZE, EventScript_UseSurfPartyMon
	goto_if_unset FLAG_BADGE05_GET, EventScript_UseSurfEnd
	checkitem ITEM_HM03, 1
	goto_if_eq VAR_RESULT, FALSE, EventScript_UseSurfEnd
	buffermovename STR_VAR_2, MOVE_SURF
	lockall
	msgbox Text_SlavelessHmPrompt, MSGBOX_YESNO
	goto_if_eq VAR_RESULT, NO, EventScript_UseSurfRelease
	setvar VAR_0x8004, FIELD_MOVE_SURF
	special Special_SetSlavelessHmSilhouette
	setfieldeffectargument 0, 0
	closemessage
	dofieldeffect FLDEFF_USE_SURF
	goto EventScript_UseSurfRelease

EventScript_UseSurfPartyMon::
	bufferpartymonnick STR_VAR_1, VAR_RESULT
	setfieldeffectargument 0, VAR_RESULT
	lockall
	msgbox Text_WantToSurf, MSGBOX_YESNO
	goto_if_eq VAR_RESULT, NO, EventScript_UseSurfRelease
	msgbox Text_UsedSurf
	dofieldeffect FLDEFF_USE_SURF
EventScript_UseSurfRelease:
	releaseall
EventScript_UseSurfEnd:
	end

EventScript_CurrentTooFast::
	lockall
	msgbox Text_CurrentTooFast
	releaseall
	end
