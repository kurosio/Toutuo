/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <antibot/antibot_data.h>

#include <engine/antibot.h>

#include <engine/shared/config.h>
#include <game/generated/server_data.h>
#include <game/mapitems.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include "character.h"
#include "laser.h"
#include "projectile.h"

#include "game/teamscore.h"
static CTeamsCore gs_TeamsCore;

MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld, CNetObj_PlayerInput LastInput) :
	CEntity(pWorld, CGameWorld::ENTTYPE_CHARACTER, vec2(0, 0), CCharacterCore::PhysicalSize())
{
	m_Health = 0;
	m_Armor = 0;
	m_StrongWeakID = 0;

	m_Input = LastInput;
	// never initialize both to zero
	m_Input.m_TargetX = 0;
	m_Input.m_TargetY = -1;

	m_LatestPrevPrevInput = m_LatestPrevInput = m_LatestInput = m_PrevInput = m_SavedInput = m_Input;
}

void CCharacter::Reset()
{
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_EmoteStop = -1;
	m_LastAction = -1;
	m_LastNoAmmoSound = -1;
	m_LastWeapon = WEAPON_HAMMER;
	m_QueuedWeapon = -1;
	m_LastRefillJumps = false;
	m_LastPenalty = false;
	m_LastBonus = false;

	m_TeleGunTeleport = false;
	m_IsBlueTeleGunTeleport = false;

	m_pPlayer = pPlayer;
	m_Pos = Pos;

	mem_zero(&m_LatestPrevPrevInput, sizeof(m_LatestPrevPrevInput));
	m_LatestPrevPrevInput.m_TargetY = -1;
	m_NumInputs = 0;
	m_SpawnTick = Server()->Tick();
	m_WeaponChangeTick = Server()->Tick();
	Antibot()->OnSpawn(m_pPlayer->GetCID());

	m_Core.Reset();
	m_Core.Init(&GameServer()->m_World.m_Core, Collision(), &gs_TeamsCore);
	m_Core.m_ActiveWeapon = WEAPON_GUN;
	m_Core.m_Pos = m_Pos;
	m_Core.m_Id = m_pPlayer->GetCID();
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = &m_Core;

	m_ReckoningTick = 0;
	m_SendCore = CCharacterCore();
	m_ReckoningCore = CCharacterCore();

	GameServer()->m_World.InsertEntity(this);
	m_Alive = true;

	GameServer()->m_pController->OnCharacterSpawn(this);

	DDRaceInit();

	m_TuneZone = Collision()->IsTune(Collision()->GetMapIndex(Pos));
	m_TuneZoneOld = -1; // no zone leave msg on spawn
	m_NeededFaketuning = 0; // reset fake tunings on respawn and send the client
	SendZoneMsgs(); // we want a entermessage also on spawn
	GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone);

	Server()->StartRecord(m_pPlayer->GetCID());

	return true;
}

void CCharacter::Destroy()
{
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	m_Alive = false;
}

void CCharacter::SetWeapon(int W)
{
	if(W == m_Core.m_ActiveWeapon)
		return;

	m_LastWeapon = m_Core.m_ActiveWeapon;
	m_QueuedWeapon = -1;
	m_Core.m_ActiveWeapon = W;
	GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH, TeamMask());

	if(m_Core.m_ActiveWeapon < 0 || m_Core.m_ActiveWeapon >= NUM_WEAPONS)
		m_Core.m_ActiveWeapon = 0;
}

bool CCharacter::IsGrounded()
{
	if(Collision()->CheckPoint(m_Pos.x + GetProximityRadius() / 2, m_Pos.y + GetProximityRadius() / 2 + 5))
		return true;
	if(Collision()->CheckPoint(m_Pos.x - GetProximityRadius() / 2, m_Pos.y + GetProximityRadius() / 2 + 5))
		return true;

	int MoveRestrictionsBelow = Collision()->GetMoveRestrictions(m_Pos + vec2(0, GetProximityRadius() / 2 + 4), 0.0f);
	return (MoveRestrictionsBelow & CANTMOVE_DOWN) != 0;
}

void CCharacter::HandleJetpack()
{
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	bool FullAuto = false;
	if(m_Core.m_ActiveWeapon == WEAPON_GRENADE || m_Core.m_ActiveWeapon == WEAPON_SHOTGUN || m_Core.m_ActiveWeapon == WEAPON_LASER)
		FullAuto = true;
	if(m_Jetpack && m_Core.m_ActiveWeapon == WEAPON_GUN)
		FullAuto = true;

	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire & 1) && m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo)
		WillFire = true;

	if(!WillFire)
		return;

	// check for ammo
	if(!m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo || m_FreezeTime)
	{
		return;
	}

	switch(m_Core.m_ActiveWeapon)
	{
	case WEAPON_GUN:
	{
		if(m_Jetpack)
		{
			float Strength;
			if(!m_TuneZone)
				Strength = GameServer()->Tuning()->m_JetpackStrength;
			else
				Strength = GameServer()->TuningList()[m_TuneZone].m_JetpackStrength;
			TakeDamage(Direction * -1.0f * (Strength / 100.0f / 6.11f), 0, m_pPlayer->GetCID(), m_Core.m_ActiveWeapon);
		}
	}
	}
}

void CCharacter::HandleNinja()
{
	if(m_Core.m_ActiveWeapon != WEAPON_NINJA)
		return;

	if((Server()->Tick() - m_Core.m_Ninja.m_ActivationTick) > (g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000))
	{
		// time's up, return
		RemoveNinja();
		return;
	}

	int NinjaTime = m_Core.m_Ninja.m_ActivationTick + (g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000) - Server()->Tick();

	if(NinjaTime % Server()->TickSpeed() == 0 && NinjaTime / Server()->TickSpeed() <= 5)
	{
		GameServer()->CreateDamageInd(m_Pos, 0, NinjaTime / Server()->TickSpeed(), TeamMask() & GameServer()->ClientsMaskExcludeClientVersionAndHigher(VERSION_DDNET_NEW_HUD));
	}

	m_Armor = clamp(10 - (NinjaTime / 15), 0, 10);

	// force ninja Weapon
	SetWeapon(WEAPON_NINJA);

	m_Core.m_Ninja.m_CurrentMoveTime--;

	if(m_Core.m_Ninja.m_CurrentMoveTime == 0)
	{
		// reset velocity
		m_Core.m_Vel = m_Core.m_Ninja.m_ActivationDir * m_Core.m_Ninja.m_OldVelAmount;
	}

	if(m_Core.m_Ninja.m_CurrentMoveTime > 0)
	{
		// Set velocity
		m_Core.m_Vel = m_Core.m_Ninja.m_ActivationDir * g_pData->m_Weapons.m_Ninja.m_Velocity;
		vec2 OldPos = m_Pos;
		Collision()->MoveBox(&m_Core.m_Pos, &m_Core.m_Vel, vec2(GetProximityRadius(), GetProximityRadius()), 0.f);

		// reset velocity so the client doesn't predict stuff
		m_Core.m_Vel = vec2(0.f, 0.f);

		// check if we Hit anything along the way
		{
			CCharacter *aEnts[MAX_CLIENTS];
			vec2 Dir = m_Pos - OldPos;
			float Radius = GetProximityRadius() * 2.0f;
			vec2 Center = OldPos + Dir * 0.5f;
			int Num = GameServer()->m_World.FindEntities(Center, Radius, (CEntity **)aEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
			for(int i = 0; i < Num; ++i)
			{
				if(aEnts[i] == this)
					continue;

				// make sure we haven't Hit this object before
				bool bAlreadyHit = false;
				for(int j = 0; j < m_NumObjectsHit; j++)
				{
					if(m_apHitObjects[j] == aEnts[i])
						bAlreadyHit = true;
				}
				if(bAlreadyHit)
					continue;

				// check so we are sufficiently close
				if(distance(aEnts[i]->m_Pos, m_Pos) > (GetProximityRadius() * 2.0f))
					continue;

				// Hit a player, give them damage and stuffs...
				GameServer()->CreateSound(aEnts[i]->m_Pos, SOUND_NINJA_HIT, TeamMask());
				// set his velocity to fast upward (for now)
				if(m_NumObjectsHit < 10)
					m_apHitObjects[m_NumObjectsHit++] = aEnts[i];

				aEnts[i]->TakeDamage(vec2(0, -10.0f), g_pData->m_Weapons.m_Ninja.m_pBase->m_Damage, m_pPlayer->GetCID(), WEAPON_NINJA);
			}
		}

		return;
	}
}

void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if(m_ReloadTimer != 0 || m_QueuedWeapon == -1 || m_Core.m_aWeapons[WEAPON_NINJA].m_Got || !m_Core.m_aWeapons[m_QueuedWeapon].m_Got)
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int WantedWeapon = m_Core.m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;

	bool Anything = false;
	for(int i = 0; i < NUM_WEAPONS - 1; ++i)
		if(m_Core.m_aWeapons[i].m_Got)
			Anything = true;
	if(!Anything)
		return;
	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon + 1) % NUM_WEAPONS;
			if(m_Core.m_aWeapons[WantedWeapon].m_Got)
				Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon - 1) < 0 ? NUM_WEAPONS - 1 : WantedWeapon - 1;
			if(m_Core.m_aWeapons[WantedWeapon].m_Got)
				Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon - 1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_Core.m_ActiveWeapon && m_Core.m_aWeapons[WantedWeapon].m_Got)
		m_QueuedWeapon = WantedWeapon;

	DoWeaponSwitch();
}

void CCharacter::FireWeapon()
{
	if(m_ReloadTimer != 0)
	{
		if(m_LatestInput.m_Fire & 1)
		{
			Antibot()->OnHammerFireReloading(m_pPlayer->GetCID());
		}
		return;
	}

	DoWeaponSwitch();
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	bool FullAuto = false;
	if(m_Core.m_ActiveWeapon == WEAPON_GRENADE || m_Core.m_ActiveWeapon == WEAPON_SHOTGUN || m_Core.m_ActiveWeapon == WEAPON_LASER)
		FullAuto = true;
	if(m_Jetpack && m_Core.m_ActiveWeapon == WEAPON_GUN)
		FullAuto = true;
	// allow firing directly after coming out of freeze or being unfrozen
	// by something
	if(m_FrozenLastTick)
		FullAuto = true;

	// don't fire hammer when player is deep and sv_deepfly is disabled
	if(!g_Config.m_SvDeepfly && m_Core.m_ActiveWeapon == WEAPON_HAMMER && m_DeepFreeze)
		return;

	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire & 1) && m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo)
		WillFire = true;

	if(!WillFire)
		return;

	if(m_FreezeTime)
	{
		// Timer stuff to avoid shrieking orchestra caused by unfreeze-plasma
		if(m_PainSoundTimer <= 0 && !(m_LatestPrevInput.m_Fire & 1))
		{
			m_PainSoundTimer = 1 * Server()->TickSpeed();
			GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG, TeamMask());
		}
		return;
	}

	// check for ammo
	if(!m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo)
		return;

	vec2 ProjStartPos = m_Pos + Direction * GetProximityRadius() * 0.75f;

	switch(m_Core.m_ActiveWeapon)
	{
	case WEAPON_HAMMER:
	{
		// reset objects Hit
		m_NumObjectsHit = 0;
		GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE, TeamMask());

		Antibot()->OnHammerFire(m_pPlayer->GetCID());

		if(m_Hit & DISABLE_HIT_HAMMER)
			break;

		CCharacter *apEnts[MAX_CLIENTS];
		int Hits = 0;
		int Num = GameServer()->m_World.FindEntities(ProjStartPos, GetProximityRadius() * 0.5f, (CEntity **)apEnts,
			MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

		for(int i = 0; i < Num; ++i)
		{
			CCharacter *pTarget = apEnts[i];

			//if ((pTarget == this) || Collision()->IntersectLine(ProjStartPos, pTarget->m_Pos, NULL, NULL))
			if((pTarget == this || (pTarget->IsAlive() && !CanCollide(pTarget->GetPlayer()->GetCID()))))
				continue;

			// set his velocity to fast upward (for now)
			if(length(pTarget->m_Pos - ProjStartPos) > 0.0f)
				GameServer()->CreateHammerHit(pTarget->m_Pos - normalize(pTarget->m_Pos - ProjStartPos) * GetProximityRadius() * 0.5f, TeamMask());
			else
				GameServer()->CreateHammerHit(ProjStartPos, TeamMask());

			vec2 Dir;
			if(length(pTarget->m_Pos - m_Pos) > 0.0f)
				Dir = normalize(pTarget->m_Pos - m_Pos);
			else
				Dir = vec2(0.f, -1.f);

			float Strength;
			if(!m_TuneZone)
				Strength = GameServer()->Tuning()->m_HammerStrength;
			else
				Strength = GameServer()->TuningList()[m_TuneZone].m_HammerStrength;

			vec2 Temp = pTarget->m_Core.m_Vel + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f;
			Temp = ClampVel(pTarget->m_MoveRestrictions, Temp);
			Temp -= pTarget->m_Core.m_Vel;
			pTarget->TakeDamage((vec2(0.f, -1.0f) + Temp) * Strength, g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage,
				m_pPlayer->GetCID(), m_Core.m_ActiveWeapon);
			pTarget->UnFreeze();

			if(m_FreezeHammer)
				pTarget->Freeze();

			Antibot()->OnHammerHit(m_pPlayer->GetCID(), pTarget->GetPlayer()->GetCID());

			Hits++;
		}

		// if we Hit anything, we have to wait for the reload
		if(Hits)
		{
			float FireDelay;
			if(!m_TuneZone)
				FireDelay = GameServer()->Tuning()->m_HammerHitFireDelay;
			else
				FireDelay = GameServer()->TuningList()[m_TuneZone].m_HammerHitFireDelay;
			m_ReloadTimer = FireDelay * Server()->TickSpeed() / 1000;
		}
	}
	break;

	case WEAPON_GUN:
	{
		if(!m_Jetpack || !m_pPlayer->m_NinjaJetpack || m_Core.m_HasTelegunGun)
		{
			int Lifetime;
			if(!m_TuneZone)
				Lifetime = (int)(Server()->TickSpeed() * GameServer()->Tuning()->m_GunLifetime);
			else
				Lifetime = (int)(Server()->TickSpeed() * GameServer()->TuningList()[m_TuneZone].m_GunLifetime);

			new CProjectile(
				GameWorld(),
				WEAPON_GUN, //Type
				m_pPlayer->GetCID(), //Owner
				ProjStartPos, //Pos
				Direction, //Dir
				Lifetime, //Span
				false, //Freeze
				false, //Explosive
				0, //Force
				-1 //SoundImpact
			);

			GameServer()->CreateSound(m_Pos, SOUND_GUN_FIRE, TeamMask());
		}
	}
	break;

	case WEAPON_SHOTGUN:
	{
		float LaserReach;
		if(!m_TuneZone)
			LaserReach = GameServer()->Tuning()->m_LaserReach;
		else
			LaserReach = GameServer()->TuningList()[m_TuneZone].m_LaserReach;

		new CLaser(&GameServer()->m_World, m_Pos, Direction, LaserReach, m_pPlayer->GetCID(), WEAPON_SHOTGUN);
		GameServer()->CreateSound(m_Pos, SOUND_SHOTGUN_FIRE, TeamMask());
	}
	break;

	case WEAPON_GRENADE:
	{
		int Lifetime;
		if(!m_TuneZone)
			Lifetime = (int)(Server()->TickSpeed() * GameServer()->Tuning()->m_GrenadeLifetime);
		else
			Lifetime = (int)(Server()->TickSpeed() * GameServer()->TuningList()[m_TuneZone].m_GrenadeLifetime);

		new CProjectile(
			GameWorld(),
			WEAPON_GRENADE, //Type
			m_pPlayer->GetCID(), //Owner
			ProjStartPos, //Pos
			Direction, //Dir
			Lifetime, //Span
			false, //Freeze
			true, //Explosive
			0, //Force
			SOUND_GRENADE_EXPLODE //SoundImpact
		); //SoundImpact

		GameServer()->CreateSound(m_Pos, SOUND_GRENADE_FIRE, TeamMask());
	}
	break;

	case WEAPON_LASER:
	{
		float LaserReach;
		if(!m_TuneZone)
			LaserReach = GameServer()->Tuning()->m_LaserReach;
		else
			LaserReach = GameServer()->TuningList()[m_TuneZone].m_LaserReach;

		new CLaser(GameWorld(), m_Pos, Direction, LaserReach, m_pPlayer->GetCID(), WEAPON_LASER);
		GameServer()->CreateSound(m_Pos, SOUND_LASER_FIRE, TeamMask());
	}
	break;

	case WEAPON_NINJA:
	{
		// reset Hit objects
		m_NumObjectsHit = 0;

		m_Core.m_Ninja.m_ActivationDir = Direction;
		m_Core.m_Ninja.m_CurrentMoveTime = g_pData->m_Weapons.m_Ninja.m_Movetime * Server()->TickSpeed() / 1000;
		m_Core.m_Ninja.m_OldVelAmount = length(m_Core.m_Vel);

		GameServer()->CreateSound(m_Pos, SOUND_NINJA_FIRE, TeamMask());
	}
	break;
	}

	m_AttackTick = Server()->Tick();

	if(!m_ReloadTimer)
	{
		float FireDelay;
		if(!m_TuneZone)
			GameServer()->Tuning()->Get(38 + m_Core.m_ActiveWeapon, &FireDelay);
		else
			GameServer()->TuningList()[m_TuneZone].Get(38 + m_Core.m_ActiveWeapon, &FireDelay);
		m_ReloadTimer = FireDelay * Server()->TickSpeed() / 1000;
	}
}

void CCharacter::HandleWeapons()
{
	//ninja
	HandleNinja();
	HandleJetpack();

	if(m_PainSoundTimer > 0)
		m_PainSoundTimer--;

	// check reload timer
	if(m_ReloadTimer)
	{
		m_ReloadTimer--;
		return;
	}

	// fire Weapon, if wanted
	FireWeapon();
}

void CCharacter::GiveNinja()
{
	m_Core.m_Ninja.m_ActivationTick = Server()->Tick();
	m_Core.m_aWeapons[WEAPON_NINJA].m_Got = true;
	m_Core.m_aWeapons[WEAPON_NINJA].m_Ammo = -1;
	if(m_Core.m_ActiveWeapon != WEAPON_NINJA)
		m_LastWeapon = m_Core.m_ActiveWeapon;
	m_Core.m_ActiveWeapon = WEAPON_NINJA;

	if(!m_Core.m_aWeapons[WEAPON_NINJA].m_Got)
		GameServer()->CreateSound(m_Pos, SOUND_PICKUP_NINJA, TeamMask());
}

void CCharacter::RemoveNinja()
{
	m_Core.m_Ninja.m_CurrentMoveTime = 0;
	m_Core.m_aWeapons[WEAPON_NINJA].m_Got = false;
	m_Core.m_ActiveWeapon = m_LastWeapon;

	SetWeapon(m_Core.m_ActiveWeapon);
}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

void CCharacter::OnPredictedInput(CNetObj_PlayerInput *pNewInput)
{
	// check for changes
	if(mem_comp(&m_SavedInput, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();

	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));

	// it is not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;

	mem_copy(&m_SavedInput, &m_Input, sizeof(m_SavedInput));
}

void CCharacter::OnDirectInput(CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));
	m_NumInputs++;

	// it is not allowed to aim in the center
	if(m_LatestInput.m_TargetX == 0 && m_LatestInput.m_TargetY == 0)
		m_LatestInput.m_TargetY = -1;

	Antibot()->OnDirectInput(m_pPlayer->GetCID());

	if(m_NumInputs > 1 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}

	mem_copy(&m_LatestPrevPrevInput, &m_LatestPrevInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::ResetHook()
{
	m_Core.SetHookedPlayer(-1);
	m_Core.m_HookState = HOOK_RETRACTED;
	m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
	m_Core.m_HookPos = m_Core.m_Pos;
}

void CCharacter::ResetInput()
{
	m_Input.m_Direction = 0;
	// simulate releasing the fire button
	if((m_Input.m_Fire & 1) != 0)
		m_Input.m_Fire++;
	m_Input.m_Fire &= INPUT_STATE_MASK;
	m_Input.m_Jump = 0;
	m_LatestPrevInput = m_LatestInput = m_Input;
}

void CCharacter::Tick()
{
	if(m_StartTime > Server()->Tick())
	{
		// Prevent the player from getting a negative time
		// The main reason why this can happen is because of time penalty tiles
		// However, other reasons are hereby also excluded
		GameServer()->SendChatTarget(m_pPlayer->GetCID(), "You died of old age");
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	}

	if(m_Paused)
		return;

	// set emote
	if(m_EmoteStop < Server()->Tick())
	{
		m_EmoteType = m_pPlayer->GetDefaultEmote();
		m_EmoteStop = -1;
	}

	DDRaceTick();

	Antibot()->OnCharacterTick(m_pPlayer->GetCID());

	m_Core.m_Input = m_Input;
	m_Core.Tick(true);

	if(!m_PrevInput.m_Hook && m_Input.m_Hook && !(m_Core.m_TriggeredEvents & COREEVENT_HOOK_ATTACH_PLAYER))
	{
		Antibot()->OnHookAttach(m_pPlayer->GetCID(), false);
	}

	// handle Weapons
	HandleWeapons();

	DDRacePostCoreTick();

	if(m_Core.m_TriggeredEvents & COREEVENT_HOOK_ATTACH_PLAYER)
	{
		if(m_Core.m_HookedPlayer != -1 && GameServer()->m_apPlayers[m_Core.m_HookedPlayer]->GetTeam() != TEAM_SPECTATORS)
		{
			Antibot()->OnHookAttach(m_pPlayer->GetCID(), true);
		}
	}

	// Previnput
	m_PrevInput = m_Input;

	m_PrevPos = m_Core.m_Pos;
}

void CCharacter::TickDefered()
{
	// advance the dummy
	{
		CWorldCore TempWorld;
		m_ReckoningCore.Init(&TempWorld, Collision(), &gs_TeamsCore, m_pTeleOuts);
		m_ReckoningCore.m_Id = m_pPlayer->GetCID();
		m_ReckoningCore.Tick(false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}

	//lastsentcore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = Collision()->TestBox(m_Core.m_Pos, CCharacterCore::PhysicalSizeVec2());

	m_Core.m_Id = m_pPlayer->GetCID();
	m_Core.Move();
	bool StuckAfterMove = Collision()->TestBox(m_Core.m_Pos, CCharacterCore::PhysicalSizeVec2());
	m_Core.Quantize();
	bool StuckAfterQuant = Collision()->TestBox(m_Core.m_Pos, CCharacterCore::PhysicalSizeVec2());
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		} StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	{
		int Events = m_Core.m_TriggeredEvents;
		int CID = m_pPlayer->GetCID();

		// Some sounds are triggered client-side for the acting player
		// so we need to avoid duplicating them
		if(Events & COREEVENT_GROUND_JUMP)
			GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, CmaskAllExceptOne(CID));

		if(Events & COREEVENT_HOOK_ATTACH_PLAYER)
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskAllExceptOne(CID));

		if(Events & COREEVENT_HOOK_ATTACH_GROUND)
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, CmaskAllExceptOne(CID));

		if(Events & COREEVENT_HOOK_HIT_NOHOOK)
			GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, CmaskAllExceptOne(CID));
	}

	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}

	// update the m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reackoning for a top of 3 seconds
		if(m_Core.m_pReset || m_ReckoningTick + Server()->TickSpeed() * 3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
			m_Core.m_pReset = false;
		}
	}
}

void CCharacter::TickPaused()
{
	++m_AttackTick;
	++m_DamageTakenTick;
	++m_Core.m_Ninja.m_ActivationTick;
	++m_ReckoningTick;
	if(m_LastAction != -1)
		++m_LastAction;
	if(m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart > -1)
		++m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_AmmoRegenStart;
	if(m_EmoteStop > -1)
		++m_EmoteStop;
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= 10)
		return false;
	m_Health = clamp(m_Health + Amount, 0, 10);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10)
		return false;
	m_Armor = clamp(m_Armor + Amount, 0, 10);
	return true;
}

void CCharacter::Die(int Killer, int Weapon)
{
	if(Server()->IsRecording(m_pPlayer->GetCID()))
		Server()->StopRecord(m_pPlayer->GetCID());

	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[Killer], Weapon);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "kill killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	// a nice sound
	GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE, TeamMask());

	// this is to rate limit respawning to 3 secs
	m_pPlayer->m_PreviousDieTick = m_pPlayer->m_DieTick;
	m_pPlayer->m_DieTick = Server()->Tick();

	m_Alive = false;

	GameServer()->m_World.RemoveEntity(this);
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID(), TeamMask());
}

bool CCharacter::TakeDamage(vec2 Force, int Dmg, int From, int Weapon)
{
	if(Dmg)
	{
		m_EmoteType = EMOTE_PAIN;
		m_EmoteStop = Server()->Tick() + 500 * Server()->TickSpeed() / 1000;
	}

	vec2 Temp = m_Core.m_Vel + Force;
	m_Core.m_Vel = ClampVel(m_MoveRestrictions, Temp);

	return true;
}

//TODO: Move the emote stuff to a function
void CCharacter::SnapCharacter(int SnappingClient, int ID)
{
	int SnappingClientVersion = SnappingClient != SERVER_DEMO_CLIENT ?
					    GameServer()->GetClientVersion(SnappingClient) :
					    CLIENT_VERSIONNR;
	CCharacterCore *pCore;
	int Tick, Emote = m_EmoteType, Weapon = m_Core.m_ActiveWeapon, AmmoCount = 0,
		  Health = 0, Armor = 0;
	if(!m_ReckoningTick || GameServer()->m_World.m_Paused)
	{
		Tick = 0;
		pCore = &m_Core;
	}
	else
	{
		Tick = m_ReckoningTick;
		pCore = &m_SendCore;
	}

	// change eyes and use ninja graphic if player is frozen
	if(m_DeepFreeze || m_FreezeTime > 0 || m_FreezeTime == -1 || m_LiveFreeze)
	{
		if(Emote == EMOTE_NORMAL)
			Emote = (m_DeepFreeze || m_LiveFreeze) ? EMOTE_PAIN : EMOTE_BLINK;

		if((m_DeepFreeze || m_FreezeTime > 0 || m_FreezeTime == -1) && SnappingClientVersion < VERSION_DDNET_NEW_HUD)
			Weapon = WEAPON_NINJA;
	}

	// This could probably happen when m_Jetpack changes instead
	// jetpack and ninjajetpack prediction
	if(m_pPlayer->GetCID() == SnappingClient)
	{
		if(m_Jetpack && Weapon != WEAPON_NINJA)
		{
			if(!(m_NeededFaketuning & FAKETUNE_JETPACK))
			{
				m_NeededFaketuning |= FAKETUNE_JETPACK;
				GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone);
			}
		}
		else
		{
			if(m_NeededFaketuning & FAKETUNE_JETPACK)
			{
				m_NeededFaketuning &= ~FAKETUNE_JETPACK;
				GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone);
			}
		}
	}

	// change eyes, use ninja graphic and set ammo count if player has ninjajetpack
	if(m_pPlayer->m_NinjaJetpack && m_Jetpack && m_Core.m_ActiveWeapon == WEAPON_GUN && !m_DeepFreeze &&
		!(m_FreezeTime > 0 || m_FreezeTime == -1) && !m_Core.m_HasTelegunGun)
	{
		if(Emote == EMOTE_NORMAL)
			Emote = EMOTE_HAPPY;
		Weapon = WEAPON_NINJA;
		AmmoCount = 10;
	}

	if(m_pPlayer->GetCID() == SnappingClient || SnappingClient == SERVER_DEMO_CLIENT ||
		(!g_Config.m_SvStrictSpectateMode && m_pPlayer->GetCID() == GameServer()->m_apPlayers[SnappingClient]->m_SpectatorID))
	{
		Health = m_Health;
		Armor = m_Armor;
		if(m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo > 0)
			AmmoCount = (!m_FreezeTime) ? m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Ammo : 0;
	}

	if(GetPlayer()->m_Afk || GetPlayer()->IsPaused())
	{
		if(m_FreezeTime > 0 || m_FreezeTime == -1 || m_DeepFreeze || m_LiveFreeze)
			Emote = EMOTE_NORMAL;
		else
			Emote = EMOTE_BLINK;
	}

	if(Emote == EMOTE_NORMAL)
	{
		if(250 - ((Server()->Tick() - m_LastAction) % (250)) < 5)
			Emote = EMOTE_BLINK;
	}

	CNetObj_Character *pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, ID, sizeof(CNetObj_Character)));
	if(!pCharacter)
		return;

	pCore->Write(pCharacter);

	pCharacter->m_Tick = Tick;
	pCharacter->m_Emote = Emote;

	if(pCharacter->m_HookedPlayer != -1)
	{
		if(!Server()->Translate(pCharacter->m_HookedPlayer, SnappingClient))
			pCharacter->m_HookedPlayer = -1;
	}

	pCharacter->m_AttackTick = m_AttackTick;
	pCharacter->m_Direction = m_Input.m_Direction;
	pCharacter->m_Weapon = Weapon;
	pCharacter->m_AmmoCount = AmmoCount;
	pCharacter->m_Health = Health;
	pCharacter->m_Armor = Armor;
	pCharacter->m_PlayerFlags = GetPlayer()->m_PlayerFlags;
}

bool CCharacter::CanSnapCharacter(int SnappingClient)
{
	if(SnappingClient == SERVER_DEMO_CLIENT)
		return true;
	return true;
}

void CCharacter::Snap(int SnappingClient)
{
	int ID = m_pPlayer->GetCID();

	if(!Server()->Translate(ID, SnappingClient))
		return;

	if(!CanSnapCharacter(SnappingClient))
	{
		return;
	}

	// A player may not be clipped away if his hook or a hook attached to him is in the field of view
	bool PlayerAndHookNotInView = NetworkClippedLine(SnappingClient, m_Pos, m_Core.m_HookPos);
	bool AttachedHookInView = false;
	if(PlayerAndHookNotInView)
	{
		for(const auto &AttachedPlayerID : m_Core.m_AttachedPlayers)
		{
			CCharacter *OtherPlayer = GameServer()->GetPlayerChar(AttachedPlayerID);
			if(OtherPlayer && OtherPlayer->m_Core.m_HookedPlayer == ID)
			{
				if(!NetworkClippedLine(SnappingClient, m_Pos, OtherPlayer->m_Pos))
				{
					AttachedHookInView = true;
					break;
				}
			}
		}
	}
	if(PlayerAndHookNotInView && !AttachedHookInView)
	{
		return;
	}

	SnapCharacter(SnappingClient, ID);

	CNetObj_DDNetCharacter *pDDNetCharacter = static_cast<CNetObj_DDNetCharacter *>(Server()->SnapNewItem(NETOBJTYPE_DDNETCHARACTER, ID, sizeof(CNetObj_DDNetCharacter)));
	if(!pDDNetCharacter)
		return;

	pDDNetCharacter->m_Flags = 0;
	if(m_Super)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_SUPER;
	if(m_EndlessHook)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_ENDLESS_HOOK;
	if(m_Core.m_NoCollision || !GameServer()->Tuning()->m_PlayerCollision)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_NO_COLLISION;
	if(m_Core.m_NoHookHit || !GameServer()->Tuning()->m_PlayerHooking)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_NO_HOOK;
	if(m_SuperJump)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_ENDLESS_JUMP;
	if(m_Jetpack)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_JETPACK;
	if(m_Hit & DISABLE_HIT_GRENADE)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_NO_GRENADE_HIT;
	if(m_Hit & DISABLE_HIT_HAMMER)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_NO_HAMMER_HIT;
	if(m_Hit & DISABLE_HIT_LASER)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_NO_LASER_HIT;
	if(m_Hit & DISABLE_HIT_SHOTGUN)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_NO_SHOTGUN_HIT;
	if(m_Core.m_HasTelegunGun)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_TELEGUN_GUN;
	if(m_Core.m_HasTelegunGrenade)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_TELEGUN_GRENADE;
	if(m_Core.m_HasTelegunLaser)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_TELEGUN_LASER;
	if(m_Core.m_aWeapons[WEAPON_HAMMER].m_Got)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_WEAPON_HAMMER;
	if(m_Core.m_aWeapons[WEAPON_GUN].m_Got)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_WEAPON_GUN;
	if(m_Core.m_aWeapons[WEAPON_SHOTGUN].m_Got)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_WEAPON_SHOTGUN;
	if(m_Core.m_aWeapons[WEAPON_GRENADE].m_Got)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_WEAPON_GRENADE;
	if(m_Core.m_aWeapons[WEAPON_LASER].m_Got)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_WEAPON_LASER;
	if(m_Core.m_ActiveWeapon == WEAPON_NINJA)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_WEAPON_NINJA;
	if(m_Core.m_LiveFrozen)
		pDDNetCharacter->m_Flags |= CHARACTERFLAG_NO_MOVEMENTS;

	pDDNetCharacter->m_FreezeEnd = m_DeepFreeze ? -1 : m_FreezeTime == 0 ? 0 : Server()->Tick() + m_FreezeTime;
	pDDNetCharacter->m_Jumps = m_Core.m_Jumps;
	pDDNetCharacter->m_TeleCheckpoint = m_TeleCheckpoint;
	pDDNetCharacter->m_StrongWeakID = m_StrongWeakID;

	CNetObj_DDNetCharacterDisplayInfo *pDDNetCharacterDisplayInfo = static_cast<CNetObj_DDNetCharacterDisplayInfo *>(Server()->SnapNewItem(NETOBJTYPE_DDNETCHARACTERDISPLAYINFO, ID, sizeof(CNetObj_DDNetCharacterDisplayInfo)));
	if(!pDDNetCharacterDisplayInfo)
		return;
	pDDNetCharacterDisplayInfo->m_JumpedTotal = m_Core.m_JumpedTotal;
	pDDNetCharacterDisplayInfo->m_NinjaActivationTick = m_Core.m_Ninja.m_ActivationTick;
	pDDNetCharacterDisplayInfo->m_FreezeTick = m_Core.m_FreezeTick;
	pDDNetCharacterDisplayInfo->m_IsInFreeze = m_Core.m_IsInFreeze;
	pDDNetCharacterDisplayInfo->m_IsInPracticeMode = -1;
	pDDNetCharacterDisplayInfo->m_TargetX = m_Core.m_Input.m_TargetX;
	pDDNetCharacterDisplayInfo->m_TargetY = m_Core.m_Input.m_TargetY;
	pDDNetCharacterDisplayInfo->m_RampValue = round_to_int(VelocityRamp(length(m_Core.m_Vel) * 50, m_Core.m_Tuning.m_VelrampStart, m_Core.m_Tuning.m_VelrampRange, m_Core.m_Tuning.m_VelrampCurvature) * 1000.0f);
}

// DDRace
bool CCharacter::CanCollide(int ClientID)
{
	return true;
}

void CCharacter::SetTeleports(std::map<int, std::vector<vec2>> *pTeleOuts, std::map<int, std::vector<vec2>> *pTeleCheckOuts)
{
	m_pTeleOuts = pTeleOuts;
	m_pTeleCheckOuts = pTeleCheckOuts;
	m_Core.m_pTeleOuts = pTeleOuts;
}

void CCharacter::FillAntibot(CAntibotCharacterData *pData)
{
	pData->m_Pos = m_Pos;
	pData->m_Vel = m_Core.m_Vel;
	pData->m_Angle = m_Core.m_Angle;
	pData->m_HookedPlayer = m_Core.m_HookedPlayer;
	pData->m_SpawnTick = m_SpawnTick;
	pData->m_WeaponChangeTick = m_WeaponChangeTick;
	pData->m_aLatestInputs[0].m_TargetX = m_LatestInput.m_TargetX;
	pData->m_aLatestInputs[0].m_TargetY = m_LatestInput.m_TargetY;
	pData->m_aLatestInputs[1].m_TargetX = m_LatestPrevInput.m_TargetX;
	pData->m_aLatestInputs[1].m_TargetY = m_LatestPrevInput.m_TargetY;
	pData->m_aLatestInputs[2].m_TargetX = m_LatestPrevPrevInput.m_TargetX;
	pData->m_aLatestInputs[2].m_TargetY = m_LatestPrevPrevInput.m_TargetY;
}

void CCharacter::HandleBroadcast()
{
}

void CCharacter::HandleSkippableTiles(int Index)
{
	// handle death-tiles and leaving gamelayer
	if(Collision()->GetCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
		   Collision()->GetCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH ||
		   Collision()->GetCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
		   Collision()->GetCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH ||
		   Collision()->GetFCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
		   Collision()->GetFCollisionAt(m_Pos.x + GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH ||
		   Collision()->GetFCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y - GetProximityRadius() / 3.f) == TILE_DEATH ||
		   Collision()->GetFCollisionAt(m_Pos.x - GetProximityRadius() / 3.f, m_Pos.y + GetProximityRadius() / 3.f) == TILE_DEATH)
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
		return;
	}

	if(GameLayerClipped(m_Pos))
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
		return;
	}

	if(Index < 0)
		return;

	// handle speedup tiles
	if(Collision()->IsSpeedup(Index))
	{
		vec2 Direction, TempVel = m_Core.m_Vel;
		int Force, MaxSpeed = 0;
		float TeeAngle, SpeederAngle, DiffAngle, SpeedLeft, TeeSpeed;
		Collision()->GetSpeedup(Index, &Direction, &Force, &MaxSpeed);
		if(Force == 255 && MaxSpeed)
		{
			m_Core.m_Vel = Direction * (MaxSpeed / 5);
		}
		else
		{
			if(MaxSpeed > 0 && MaxSpeed < 5)
				MaxSpeed = 5;
			if(MaxSpeed > 0)
			{
				if(Direction.x > 0.0000001f)
					SpeederAngle = -atan(Direction.y / Direction.x);
				else if(Direction.x < 0.0000001f)
					SpeederAngle = atan(Direction.y / Direction.x) + 2.0f * asin(1.0f);
				else if(Direction.y > 0.0000001f)
					SpeederAngle = asin(1.0f);
				else
					SpeederAngle = asin(-1.0f);

				if(SpeederAngle < 0)
					SpeederAngle = 4.0f * asin(1.0f) + SpeederAngle;

				if(TempVel.x > 0.0000001f)
					TeeAngle = -atan(TempVel.y / TempVel.x);
				else if(TempVel.x < 0.0000001f)
					TeeAngle = atan(TempVel.y / TempVel.x) + 2.0f * asin(1.0f);
				else if(TempVel.y > 0.0000001f)
					TeeAngle = asin(1.0f);
				else
					TeeAngle = asin(-1.0f);

				if(TeeAngle < 0)
					TeeAngle = 4.0f * asin(1.0f) + TeeAngle;

				TeeSpeed = sqrt(pow(TempVel.x, 2) + pow(TempVel.y, 2));

				DiffAngle = SpeederAngle - TeeAngle;
				SpeedLeft = MaxSpeed / 5.0f - cos(DiffAngle) * TeeSpeed;
				if(abs((int)SpeedLeft) > Force && SpeedLeft > 0.0000001f)
					TempVel += Direction * Force;
				else if(abs((int)SpeedLeft) > Force)
					TempVel += Direction * -Force;
				else
					TempVel += Direction * SpeedLeft;
			}
			else
				TempVel += Direction * Force;

			m_Core.m_Vel = ClampVel(m_MoveRestrictions, TempVel);
		}
	}
}

bool CCharacter::IsSwitchActiveCb(int Number, void *pUser)
{
	CCharacter *pThis = (CCharacter *)pUser;
	auto &aSwitchers = pThis->Switchers();
	return !aSwitchers.empty() && aSwitchers[Number].m_Status[pThis->EventGroup()];
}

void CCharacter::HandleTiles(int Index)
{
	int MapIndex = Index;
	//int PureMapIndex = Collision()->GetPureMapIndex(m_Pos);
	m_TileIndex = Collision()->GetTileIndex(MapIndex);
	m_TileFIndex = Collision()->GetFTileIndex(MapIndex);
	m_MoveRestrictions = Collision()->GetMoveRestrictions(IsSwitchActiveCb, this, m_Pos, 18.0f, MapIndex);
	if(Index < 0)
	{
		m_LastRefillJumps = false;
		m_LastPenalty = false;
		m_LastBonus = false;
		return;
	}

	int tcp = Collision()->IsTCheckpoint(MapIndex);
	if(tcp)
		m_TeleCheckpoint = tcp;

	GameServer()->m_pController->HandleCharacterTiles(this, Index);

	// freeze
	if(((m_TileIndex == TILE_FREEZE) || (m_TileFIndex == TILE_FREEZE)) && !m_Super && !m_DeepFreeze)
	{
		Freeze();
	}
	else if(((m_TileIndex == TILE_UNFREEZE) || (m_TileFIndex == TILE_UNFREEZE)) && !m_DeepFreeze)
		UnFreeze();

	// deep freeze
	if(((m_TileIndex == TILE_DFREEZE) || (m_TileFIndex == TILE_DFREEZE)) && !m_Super && !m_DeepFreeze)
		m_DeepFreeze = true;
	else if(((m_TileIndex == TILE_DUNFREEZE) || (m_TileFIndex == TILE_DUNFREEZE)) && !m_Super && m_DeepFreeze)
		m_DeepFreeze = false;

	// endless hook
	if(((m_TileIndex == TILE_EHOOK_ENABLE) || (m_TileFIndex == TILE_EHOOK_ENABLE)))
	{
		SetEndlessHook(true);
	}
	else if(((m_TileIndex == TILE_EHOOK_DISABLE) || (m_TileFIndex == TILE_EHOOK_DISABLE)))
	{
		SetEndlessHook(false);
	}

	// hit others
	if(((m_TileIndex == TILE_HIT_DISABLE) || (m_TileFIndex == TILE_HIT_DISABLE)) && m_Hit != (DISABLE_HIT_GRENADE | DISABLE_HIT_HAMMER | DISABLE_HIT_LASER | DISABLE_HIT_SHOTGUN))
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can't hit others");
		}
		m_Hit = DISABLE_HIT_GRENADE | DISABLE_HIT_HAMMER | DISABLE_HIT_LASER | DISABLE_HIT_SHOTGUN;
		m_Core.m_NoShotgunHit = true;
		m_Core.m_NoGrenadeHit = true;
		m_Core.m_NoHammerHit = true;
		m_Core.m_NoLaserHit = true;
		m_NeededFaketuning |= FAKETUNE_NOHAMMER;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}
	else if(((m_TileIndex == TILE_HIT_ENABLE) || (m_TileFIndex == TILE_HIT_ENABLE)) && m_Hit != HIT_ALL)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can hit others");
		}
		m_Hit = HIT_ALL;
		m_Core.m_NoShotgunHit = false;
		m_Core.m_NoGrenadeHit = false;
		m_Core.m_NoHammerHit = false;
		m_Core.m_NoLaserHit = false;
		m_NeededFaketuning &= ~FAKETUNE_NOHAMMER;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}

	// collide with others
	if(((m_TileIndex == TILE_NPC_DISABLE) || (m_TileFIndex == TILE_NPC_DISABLE)) && !m_Core.m_NoCollision)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can't collide with others");
		}
		m_Core.m_NoCollision = true;
		m_NeededFaketuning |= FAKETUNE_NOCOLL;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}
	else if(((m_TileIndex == TILE_NPC_ENABLE) || (m_TileFIndex == TILE_NPC_ENABLE)) && m_Core.m_NoCollision)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can collide with others");
		}
		m_Core.m_NoCollision = false;
		m_NeededFaketuning &= ~FAKETUNE_NOCOLL;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}

	// hook others
	if(((m_TileIndex == TILE_NPH_DISABLE) || (m_TileFIndex == TILE_NPH_DISABLE)) && !m_Core.m_NoHookHit)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can't hook others");
		}
		m_Core.m_NoHookHit = true;
		m_NeededFaketuning |= FAKETUNE_NOHOOK;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}
	else if(((m_TileIndex == TILE_NPH_ENABLE) || (m_TileFIndex == TILE_NPH_ENABLE)) && m_Core.m_NoHookHit)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can hook others");
		}
		m_Core.m_NoHookHit = false;
		m_NeededFaketuning &= ~FAKETUNE_NOHOOK;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}

	// unlimited air jumps
	if(((m_TileIndex == TILE_UNLIMITED_JUMPS_ENABLE) || (m_TileFIndex == TILE_UNLIMITED_JUMPS_ENABLE)) && !m_SuperJump)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You have unlimited air jumps");
		}
		m_SuperJump = true;
		m_Core.m_EndlessJump = true;
		if(m_Core.m_Jumps == 0)
		{
			m_NeededFaketuning &= ~FAKETUNE_NOJUMP;
			GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
		}
	}
	else if(((m_TileIndex == TILE_UNLIMITED_JUMPS_DISABLE) || (m_TileFIndex == TILE_UNLIMITED_JUMPS_DISABLE)) && m_SuperJump)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You don't have unlimited air jumps");
		}
		m_SuperJump = false;
		m_Core.m_EndlessJump = false;
		if(m_Core.m_Jumps == 0)
		{
			m_NeededFaketuning |= FAKETUNE_NOJUMP;
			GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
		}
	}

	// walljump
	if((m_TileIndex == TILE_WALLJUMP) || (m_TileFIndex == TILE_WALLJUMP))
	{
		if(m_Core.m_Vel.y > 0 && m_Core.m_Colliding && m_Core.m_LeftWall)
		{
			m_Core.m_LeftWall = false;
			m_Core.m_JumpedTotal = m_Core.m_Jumps >= 2 ? m_Core.m_Jumps - 2 : 0;
			m_Core.m_Jumped = 1;
		}
	}

	// jetpack gun
	if(((m_TileIndex == TILE_JETPACK_ENABLE) || (m_TileFIndex == TILE_JETPACK_ENABLE)) && !m_Jetpack)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You have a jetpack gun");
		}
		m_Jetpack = true;
		m_Core.m_Jetpack = true;
	}
	else if(((m_TileIndex == TILE_JETPACK_DISABLE) || (m_TileFIndex == TILE_JETPACK_DISABLE)) && m_Jetpack)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You lost your jetpack gun");
		}
		m_Jetpack = false;
		m_Core.m_Jetpack = false;
	}

	// refill jumps
	if(((m_TileIndex == TILE_REFILL_JUMPS) || (m_TileFIndex == TILE_REFILL_JUMPS)) && !m_LastRefillJumps)
	{
		m_Core.m_JumpedTotal = 0;
		m_Core.m_Jumped = 0;
		m_LastRefillJumps = true;
	}
	if((m_TileIndex != TILE_REFILL_JUMPS) && (m_TileFIndex != TILE_REFILL_JUMPS))
	{
		m_LastRefillJumps = false;
	}

	// Teleport gun
	if(((m_TileIndex == TILE_TELE_GUN_ENABLE) || (m_TileFIndex == TILE_TELE_GUN_ENABLE)) && !m_Core.m_HasTelegunGun)
	{
		m_Core.m_HasTelegunGun = true;
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "Teleport gun enabled");
		}
	}
	else if(((m_TileIndex == TILE_TELE_GUN_DISABLE) || (m_TileFIndex == TILE_TELE_GUN_DISABLE)) && m_Core.m_HasTelegunGun)
	{
		m_Core.m_HasTelegunGun = false;
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "Teleport gun disabled");
		}
	}

	if(((m_TileIndex == TILE_TELE_GRENADE_ENABLE) || (m_TileFIndex == TILE_TELE_GRENADE_ENABLE)) && !m_Core.m_HasTelegunGrenade)
	{
		m_Core.m_HasTelegunGrenade = true;
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "Teleport grenade enabled");
		}
	}
	else if(((m_TileIndex == TILE_TELE_GRENADE_DISABLE) || (m_TileFIndex == TILE_TELE_GRENADE_DISABLE)) && m_Core.m_HasTelegunGrenade)
	{
		m_Core.m_HasTelegunGrenade = false;
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "Teleport grenade disabled");
		}
	}

	if(((m_TileIndex == TILE_TELE_LASER_ENABLE) || (m_TileFIndex == TILE_TELE_LASER_ENABLE)) && !m_Core.m_HasTelegunLaser)
	{
		m_Core.m_HasTelegunLaser = true;
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "Teleport laser enabled");
		}
	}
	else if(((m_TileIndex == TILE_TELE_LASER_DISABLE) || (m_TileFIndex == TILE_TELE_LASER_DISABLE)) && m_Core.m_HasTelegunLaser)
	{
		m_Core.m_HasTelegunLaser = false;
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "Teleport laser disabled");
		}
	}

	// stopper
	if(m_Core.m_Vel.y > 0 && (m_MoveRestrictions & CANTMOVE_DOWN))
	{
		m_Core.m_Jumped = 0;
		m_Core.m_JumpedTotal = 0;
	}
	m_Core.m_Vel = ClampVel(m_MoveRestrictions, m_Core.m_Vel);

	// handle switch tiles
	if(Collision()->GetSwitchType(MapIndex) == TILE_SWITCHOPEN && Collision()->GetSwitchNumber(MapIndex) > 0)
	{
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()] = true;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_EndTick[EventGroup()] = 0;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Type[EventGroup()] = TILE_SWITCHOPEN;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_LastUpdateTick[EventGroup()] = Server()->Tick();
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_SWITCHTIMEDOPEN && Collision()->GetSwitchNumber(MapIndex) > 0)
	{
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()] = true;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_EndTick[EventGroup()] = Server()->Tick() + 1 + Collision()->GetSwitchDelay(MapIndex) * Server()->TickSpeed();
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Type[EventGroup()] = TILE_SWITCHTIMEDOPEN;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_LastUpdateTick[EventGroup()] = Server()->Tick();
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_SWITCHTIMEDCLOSE && Collision()->GetSwitchNumber(MapIndex) > 0)
	{
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()] = false;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_EndTick[EventGroup()] = Server()->Tick() + 1 + Collision()->GetSwitchDelay(MapIndex) * Server()->TickSpeed();
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Type[EventGroup()] = TILE_SWITCHTIMEDCLOSE;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_LastUpdateTick[EventGroup()] = Server()->Tick();
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_SWITCHCLOSE && Collision()->GetSwitchNumber(MapIndex) > 0)
	{
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()] = false;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_EndTick[EventGroup()] = 0;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Type[EventGroup()] = TILE_SWITCHCLOSE;
		Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_LastUpdateTick[EventGroup()] = Server()->Tick();
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_FREEZE)
	{
		if(Collision()->GetSwitchNumber(MapIndex) == 0 || Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()])
		{
			Freeze(Collision()->GetSwitchDelay(MapIndex));
		}
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_DFREEZE)
	{
		if(Collision()->GetSwitchNumber(MapIndex) == 0 || Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()])
			m_DeepFreeze = true;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_DUNFREEZE)
	{
		if(Collision()->GetSwitchNumber(MapIndex) == 0 || Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()])
			m_DeepFreeze = false;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_LFREEZE)
	{
		if(Collision()->GetSwitchNumber(MapIndex) == 0 || Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()])
		{
			m_LiveFreeze = true;
			m_Core.m_LiveFrozen = true;
		}
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_LUNFREEZE)
	{
		if(Collision()->GetSwitchNumber(MapIndex) == 0 || Switchers()[Collision()->GetSwitchNumber(MapIndex)].m_Status[EventGroup()])
		{
			m_LiveFreeze = false;
			m_Core.m_LiveFrozen = false;
		}
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_ENABLE && m_Hit & DISABLE_HIT_HAMMER && Collision()->GetSwitchDelay(MapIndex) == WEAPON_HAMMER)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can hammer hit others");
		}
		m_Hit &= ~DISABLE_HIT_HAMMER;
		m_NeededFaketuning &= ~FAKETUNE_NOHAMMER;
		m_Core.m_NoHammerHit = false;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_DISABLE && !(m_Hit & DISABLE_HIT_HAMMER) && Collision()->GetSwitchDelay(MapIndex) == WEAPON_HAMMER)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can't hammer hit others");
		}
		m_Hit |= DISABLE_HIT_HAMMER;
		m_NeededFaketuning |= FAKETUNE_NOHAMMER;
		m_Core.m_NoHammerHit = true;
		GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_ENABLE && m_Hit & DISABLE_HIT_SHOTGUN && Collision()->GetSwitchDelay(MapIndex) == WEAPON_SHOTGUN)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can shoot others with shotgun");
		}
		m_Hit &= ~DISABLE_HIT_SHOTGUN;
		m_Core.m_NoShotgunHit = false;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_DISABLE && !(m_Hit & DISABLE_HIT_SHOTGUN) && Collision()->GetSwitchDelay(MapIndex) == WEAPON_SHOTGUN)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can't shoot others with shotgun");
		}
		m_Hit |= DISABLE_HIT_SHOTGUN;
		m_Core.m_NoShotgunHit = true;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_ENABLE && m_Hit & DISABLE_HIT_GRENADE && Collision()->GetSwitchDelay(MapIndex) == WEAPON_GRENADE)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can shoot others with grenade");
		}
		m_Hit &= ~DISABLE_HIT_GRENADE;
		m_Core.m_NoGrenadeHit = false;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_DISABLE && !(m_Hit & DISABLE_HIT_GRENADE) && Collision()->GetSwitchDelay(MapIndex) == WEAPON_GRENADE)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can't shoot others with grenade");
		}
		m_Hit |= DISABLE_HIT_GRENADE;
		m_Core.m_NoGrenadeHit = true;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_ENABLE && m_Hit & DISABLE_HIT_LASER && Collision()->GetSwitchDelay(MapIndex) == WEAPON_LASER)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can shoot others with laser");
		}
		m_Hit &= ~DISABLE_HIT_LASER;
		m_Core.m_NoLaserHit = false;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_HIT_DISABLE && !(m_Hit & DISABLE_HIT_LASER) && Collision()->GetSwitchDelay(MapIndex) == WEAPON_LASER)
	{
		if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
		{
			GameServer()->SendChatTarget(GetPlayer()->GetCID(), "You can't shoot others with laser");
		}
		m_Hit |= DISABLE_HIT_LASER;
		m_Core.m_NoLaserHit = true;
	}
	else if(Collision()->GetSwitchType(MapIndex) == TILE_JUMP)
	{
		int NewJumps = Collision()->GetSwitchDelay(MapIndex);
		if(NewJumps == 255)
		{
			NewJumps = -1;
		}

		if(NewJumps != m_Core.m_Jumps)
		{
			if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
			{
				char aBuf[256];
				if(NewJumps == -1)
					str_format(aBuf, sizeof(aBuf), "You only have your ground jump now");
				else if(NewJumps == 1)
					str_format(aBuf, sizeof(aBuf), "You can jump %d time", NewJumps);
				else
					str_format(aBuf, sizeof(aBuf), "You can jump %d times", NewJumps);
				GameServer()->SendChatTarget(GetPlayer()->GetCID(), aBuf);
			}
			if(NewJumps == 0 && !m_SuperJump)
			{
				m_NeededFaketuning |= FAKETUNE_NOJUMP;
				GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
			}
			else if(m_Core.m_Jumps == 0)
			{
				m_NeededFaketuning &= ~FAKETUNE_NOJUMP;
				GameServer()->SendTuningParams(m_pPlayer->GetCID(), m_TuneZone); // update tunings
			}

			m_Core.m_Jumps = NewJumps;
		}
	}

	int z = Collision()->IsTeleport(MapIndex);
	if(!g_Config.m_SvOldTeleportHook && !g_Config.m_SvOldTeleportWeapons && z && !(*m_pTeleOuts)[z - 1].empty())
	{
		if(m_Super)
			return;
		int TeleOut = m_Core.m_pWorld->RandomOr0((*m_pTeleOuts)[z - 1].size());
		m_Core.m_Pos = (*m_pTeleOuts)[z - 1][TeleOut];
		if(!g_Config.m_SvTeleportHoldHook)
		{
			ResetHook();
		}
		if(g_Config.m_SvTeleportLoseWeapons)
			ResetPickups();
		return;
	}
	int evilz = Collision()->IsEvilTeleport(MapIndex);
	if(evilz && !(*m_pTeleOuts)[evilz - 1].empty())
	{
		if(m_Super)
			return;
		int TeleOut = m_Core.m_pWorld->RandomOr0((*m_pTeleOuts)[evilz - 1].size());
		m_Core.m_Pos = (*m_pTeleOuts)[evilz - 1][TeleOut];
		if(!g_Config.m_SvOldTeleportHook && !g_Config.m_SvOldTeleportWeapons)
		{
			m_Core.m_Vel = vec2(0, 0);

			if(!g_Config.m_SvTeleportHoldHook)
			{
				ResetHook();
				GameWorld()->ReleaseHooked(GetPlayer()->GetCID());
			}
			if(g_Config.m_SvTeleportLoseWeapons)
			{
				ResetPickups();
			}
		}
		return;
	}
	if(Collision()->IsCheckEvilTeleport(MapIndex))
	{
		if(m_Super)
			return;
		// first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous checkpoints
		for(int k = m_TeleCheckpoint - 1; k >= 0; k--)
		{
			if(!(*m_pTeleCheckOuts)[k].empty())
			{
				int TeleOut = m_Core.m_pWorld->RandomOr0((*m_pTeleCheckOuts)[k].size());
				m_Core.m_Pos = (*m_pTeleCheckOuts)[k][TeleOut];
				m_Core.m_Vel = vec2(0, 0);

				if(!g_Config.m_SvTeleportHoldHook)
				{
					ResetHook();
					GameWorld()->ReleaseHooked(GetPlayer()->GetCID());
				}

				return;
			}
		}
		// if no checkpointout have been found (or if there no recorded checkpoint), teleport to start
		vec2 SpawnPos;
		if(GameServer()->m_pController->CanSpawn(m_pPlayer->GetTeam(), &SpawnPos, 0))
		{
			m_Core.m_Pos = SpawnPos;
			m_Core.m_Vel = vec2(0, 0);

			if(!g_Config.m_SvTeleportHoldHook)
			{
				ResetHook();
				GameWorld()->ReleaseHooked(GetPlayer()->GetCID());
			}
		}
		return;
	}
	if(Collision()->IsCheckTeleport(MapIndex))
	{
		if(m_Super)
			return;
		// first check if there is a TeleCheckOut for the current recorded checkpoint, if not check previous checkpoints
		for(int k = m_TeleCheckpoint - 1; k >= 0; k--)
		{
			if(!(*m_pTeleCheckOuts)[k].empty())
			{
				int TeleOut = m_Core.m_pWorld->RandomOr0((*m_pTeleCheckOuts)[k].size());
				m_Core.m_Pos = (*m_pTeleCheckOuts)[k][TeleOut];

				if(!g_Config.m_SvTeleportHoldHook)
				{
					ResetHook();
				}

				return;
			}
		}
		// if no checkpointout have been found (or if there no recorded checkpoint), teleport to start
		vec2 SpawnPos;
		if(GameServer()->m_pController->CanSpawn(m_pPlayer->GetTeam(), &SpawnPos, 0))
		{
			m_Core.m_Pos = SpawnPos;

			if(!g_Config.m_SvTeleportHoldHook)
			{
				ResetHook();
			}
		}
		return;
	}
}

void CCharacter::HandleTuneLayer()
{
	m_TuneZoneOld = m_TuneZone;
	int CurrentIndex = Collision()->GetMapIndex(m_Pos);
	m_TuneZone = Collision()->IsTune(CurrentIndex);

	if(m_TuneZone)
		m_Core.m_Tuning = GameServer()->TuningList()[m_TuneZone]; // throw tunings from specific zone into gamecore
	else
		m_Core.m_Tuning = *GameServer()->Tuning();

	if(m_TuneZone != m_TuneZoneOld) // don't send tunigs all the time
	{
		// send zone msgs
		SendZoneMsgs();
	}
}

void CCharacter::SendZoneMsgs()
{
	// send zone leave msg
	// (m_TuneZoneOld >= 0: avoid zone leave msgs on spawn)
	if(m_TuneZoneOld >= 0 && GameServer()->m_aaZoneLeaveMsg[m_TuneZoneOld][0])
	{
		const char *pCur = GameServer()->m_aaZoneLeaveMsg[m_TuneZoneOld];
		const char *pPos;
		while((pPos = str_find(pCur, "\\n")))
		{
			char aBuf[256];
			str_copy(aBuf, pCur, pPos - pCur + 1);
			aBuf[pPos - pCur + 1] = '\0';
			pCur = pPos + 2;
			GameServer()->SendChatTarget(m_pPlayer->GetCID(), aBuf);
		}
		GameServer()->SendChatTarget(m_pPlayer->GetCID(), pCur);
	}
	// send zone enter msg
	if(GameServer()->m_aaZoneEnterMsg[m_TuneZone][0])
	{
		const char *pCur = GameServer()->m_aaZoneEnterMsg[m_TuneZone];
		const char *pPos;
		while((pPos = str_find(pCur, "\\n")))
		{
			char aBuf[256];
			str_copy(aBuf, pCur, pPos - pCur + 1);
			aBuf[pPos - pCur + 1] = '\0';
			pCur = pPos + 2;
			GameServer()->SendChatTarget(m_pPlayer->GetCID(), aBuf);
		}
		GameServer()->SendChatTarget(m_pPlayer->GetCID(), pCur);
	}
}

IAntibot *CCharacter::Antibot()
{
	return GameServer()->Antibot();
}

void CCharacter::DDRaceTick()
{
	mem_copy(&m_Input, &m_SavedInput, sizeof(m_Input));
	m_Armor = (m_FreezeTime >= 0) ? clamp(10 - (m_FreezeTime / 15), 0, 10) : 0;
	if(m_Input.m_Direction != 0 || m_Input.m_Jump != 0)
		m_LastMove = Server()->Tick();

	if(m_LiveFreeze && !m_Super)
	{
		m_Input.m_Direction = 0;
		m_Input.m_Jump = 0;
		// Hook is possible in live freeze
	}
	if(m_FreezeTime > 0 || m_FreezeTime == -1)
	{
		if(m_FreezeTime % Server()->TickSpeed() == Server()->TickSpeed() - 1 || m_FreezeTime == -1)
		{
			GameServer()->CreateDamageInd(m_Pos, 0, (m_FreezeTime + 1) / Server()->TickSpeed(), TeamMask() & GameServer()->ClientsMaskExcludeClientVersionAndHigher(VERSION_DDNET_NEW_HUD));
		}
		if(m_FreezeTime > 0)
			m_FreezeTime--;
		else
			m_Core.m_Ninja.m_ActivationTick = Server()->Tick();
		m_Input.m_Direction = 0;
		m_Input.m_Jump = 0;
		m_Input.m_Hook = 0;
		if(m_FreezeTime == 1)
			UnFreeze();
	}

	HandleTuneLayer(); // need this before coretick

	// check if the tee is in any type of freeze
	int Index = Collision()->GetPureMapIndex(m_Pos);
	const int aTiles[] = {
		Collision()->GetTileIndex(Index),
		Collision()->GetFTileIndex(Index),
		Collision()->GetSwitchType(Index)};
	m_Core.m_IsInFreeze = false;
	for(const int Tile : aTiles)
	{
		if(Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE)
		{
			m_Core.m_IsInFreeze = true;
			break;
		}
	}

	m_Core.m_Id = GetPlayer()->GetCID();
}

void CCharacter::DDRacePostCoreTick()
{
	m_Time = (float)(Server()->Tick() - m_StartTime) / ((float)Server()->TickSpeed());

	if(m_EndlessHook || (m_Super && g_Config.m_SvEndlessSuperHook))
		m_Core.m_HookTick = 0;

	m_FrozenLastTick = false;

	if(m_DeepFreeze && !m_Super)
		Freeze();

	// following jump rules can be overridden by tiles, like Refill Jumps, Stopper and Wall Jump
	if(m_Core.m_Jumps == -1)
	{
		// The player has only one ground jump, so his feet are always dark
		m_Core.m_Jumped |= 2;
	}
	else if(m_Core.m_Jumps == 0)
	{
		// The player has no jumps at all, so his feet are always dark
		m_Core.m_Jumped |= 2;
	}
	else if(m_Core.m_Jumps == 1 && m_Core.m_Jumped > 0)
	{
		// If the player has only one jump, each jump is the last one
		m_Core.m_Jumped |= 2;
	}
	else if(m_Core.m_JumpedTotal < m_Core.m_Jumps - 1 && m_Core.m_Jumped > 1)
	{
		// The player has not yet used up all his jumps, so his feet remain light
		m_Core.m_Jumped = 1;
	}

	if((m_Super || m_SuperJump) && m_Core.m_Jumped > 1)
	{
		// Super players and players with infinite jumps always have light feet
		m_Core.m_Jumped = 1;
	}

	int CurrentIndex = Collision()->GetMapIndex(m_Pos);
	HandleSkippableTiles(CurrentIndex);
	if(!m_Alive)
		return;

	// handle Anti-Skip tiles
	std::list<int> Indices = Collision()->GetMapIndices(m_PrevPos, m_Pos);
	if(!Indices.empty())
	{
		for(int &Index : Indices)
		{
			HandleTiles(Index);
			if(!m_Alive)
				return;
		}
	}
	else
	{
		HandleTiles(CurrentIndex);
		if(!m_Alive)
			return;
	}

	// teleport gun
	if(m_TeleGunTeleport)
	{
		GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID(), TeamMask());
		m_Core.m_Pos = m_TeleGunPos;
		if(!m_IsBlueTeleGunTeleport)
			m_Core.m_Vel = vec2(0, 0);
		GameServer()->CreateDeath(m_TeleGunPos, m_pPlayer->GetCID(), TeamMask());
		GameServer()->CreateSound(m_TeleGunPos, SOUND_WEAPON_SPAWN, TeamMask());
		m_TeleGunTeleport = false;
		m_IsBlueTeleGunTeleport = false;
	}

	HandleBroadcast();
}

bool CCharacter::Freeze(int Seconds)
{
	if((Seconds <= 0 || m_Super || m_FreezeTime == -1 || m_FreezeTime > Seconds * Server()->TickSpeed()) && Seconds != -1)
		return false;
	if(m_Core.m_FreezeTick < Server()->Tick() - Server()->TickSpeed() || Seconds == -1)
	{
		m_Armor = 0;
		m_FreezeTime = Seconds == -1 ? Seconds : Seconds * Server()->TickSpeed();
		m_Core.m_FreezeTick = Server()->Tick();
		return true;
	}
	return false;
}

bool CCharacter::Freeze()
{
	return Freeze(g_Config.m_SvFreezeDelay);
}

bool CCharacter::UnFreeze()
{
	if(m_FreezeTime > 0)
	{
		m_Armor = 10;
		if(!m_Core.m_aWeapons[m_Core.m_ActiveWeapon].m_Got)
			m_Core.m_ActiveWeapon = WEAPON_GUN;
		m_FreezeTime = 0;
		m_Core.m_FreezeTick = 0;
		m_FrozenLastTick = true;
		return true;
	}
	return false;
}

void CCharacter::GiveWeapon(int Weapon, bool Remove)
{
	if(Weapon == WEAPON_NINJA)
	{
		if(Remove)
			RemoveNinja();
		else
			GiveNinja();
		return;
	}

	if(Remove)
	{
		if(GetActiveWeapon() == Weapon)
			SetActiveWeapon(WEAPON_GUN);
	}
	else
	{
		m_Core.m_aWeapons[Weapon].m_Ammo = -1;
	}

	m_Core.m_aWeapons[Weapon].m_Got = !Remove;
}

void CCharacter::GiveAllWeapons()
{
	for(int i = WEAPON_GUN; i < NUM_WEAPONS - 1; i++)
	{
		GiveWeapon(i);
	}
}

void CCharacter::ResetPickups()
{
	for(int i = WEAPON_SHOTGUN; i < NUM_WEAPONS - 1; i++)
	{
		m_Core.m_aWeapons[i].m_Got = false;
		if(m_Core.m_ActiveWeapon == i)
			m_Core.m_ActiveWeapon = WEAPON_GUN;
	}
}

void CCharacter::SetEndlessHook(bool Enable)
{
	if(m_EndlessHook == Enable)
	{
		return;
	}
	if(GameServer()->GetClientVersion(GetPlayer()->GetCID()) < VERSION_DDNET_NEW_HUD)
	{
		GameServer()->SendChatTarget(GetPlayer()->GetCID(), Enable ? "Endless hook has been activated" : "Endless hook has been deactivated");
	}
	m_EndlessHook = Enable;
	m_Core.m_EndlessHook = Enable;
}

void CCharacter::Pause(bool Pause)
{
	m_Paused = Pause;
	if(Pause)
	{
		GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
		GameServer()->m_World.RemoveEntity(this);

		if(m_Core.m_HookedPlayer != -1) // Keeping hook would allow cheats
		{
			ResetHook();
			GameWorld()->ReleaseHooked(GetPlayer()->GetCID());
		}
	}
	else
	{
		m_Core.m_Vel = vec2(0, 0);
		GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = &m_Core;
		GameServer()->m_World.InsertEntity(this);
	}
}

void CCharacter::DDRaceInit()
{
	m_Paused = false;
	m_DDRaceState = DDRACE_NONE;
	m_PrevPos = m_Pos;
	m_LastBroadcast = 0;
	m_TeamBeforeSuper = 0;
	m_Core.m_Id = GetPlayer()->GetCID();
	m_TeleCheckpoint = 0;
	m_EndlessHook = g_Config.m_SvEndlessDrag;
	m_Hit = g_Config.m_SvHit ? HIT_ALL : DISABLE_HIT_GRENADE | DISABLE_HIT_HAMMER | DISABLE_HIT_LASER | DISABLE_HIT_SHOTGUN;
	m_SuperJump = false;
	m_Jetpack = false;
	m_Core.m_Jumps = 2;
	m_FreezeHammer = false;
}

int64_t CCharacter::TeamMask()
{
	return -1;
}

void CCharacter::SwapClients(int Client1, int Client2)
{
	m_Core.SetHookedPlayer(m_Core.m_HookedPlayer == Client1 ? Client2 : m_Core.m_HookedPlayer == Client2 ? Client1 : m_Core.m_HookedPlayer);
}