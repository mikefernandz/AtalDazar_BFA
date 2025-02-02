/*
 * Copyright (C) 2008-2019 TrinityCore <https://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CombatAI.h"
#include "ConditionMgr.h"
#include "Creature.h"
#include "CreatureAIImpl.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Vehicle.h"

/////////////////
// AggressorAI
/////////////////

int AggressorAI::Permissible(const Creature* creature)
{
    // have some hostile factions, it will be selected by IsHostileTo check at MoveInLineOfSight
    if (!creature->IsCivilian() && !creature->IsNeutralToAll())
        return PERMIT_BASE_PROACTIVE;

    return PERMIT_BASE_NO;
}

void AggressorAI::UpdateAI(uint32 /*diff*/)
{
    if (!UpdateVictim())
        return;

    DoMeleeAttackIfReady();
}

/////////////////
// CombatAI
/////////////////

void CombatAI::InitializeAI()
{
    for (uint32 i = 0; i < MAX_CREATURE_SPELLS; ++i)
        if (me->m_spells[i] && sSpellMgr->GetSpellInfo(me->m_spells[i]))
            spells.push_back(me->m_spells[i]);

    CreatureAI::InitializeAI();
}

void CombatAI::Reset()
{
    events.Reset();
    spellEvents.Reset();

    events.ScheduleEvent(EVENT_UPDATE_VICTIM, 0ms, 500ms);
}

void CombatAI::JustDied(Unit* killer)
{
    for (SpellVct::iterator i = spells.begin(); i != spells.end(); ++i)
        if (AISpellInfo[*i].condition == AICOND_DIE)
            me->CastSpell(killer, *i, true);
}

void CombatAI::EnterCombat(Unit* who)
{
    for (SpellVct::iterator i = spells.begin(); i != spells.end(); ++i)
    {
        if (AISpellInfo[*i].condition == AICOND_AGGRO)
            me->CastSpell(who, *i, false);
        else if (AISpellInfo[*i].condition == AICOND_COMBAT)
            spellEvents.ScheduleEvent(*i, AISpellInfo[*i].cooldown + rand32() % AISpellInfo[*i].cooldown);
    }
}

void CombatAI::UpdateAI(uint32 diff)
{
    events.Update(diff);

    if (events.ExecuteEvent() == EVENT_UPDATE_VICTIM)
    {
        if (!UpdateVictim())
        {
            events.Repeat(500ms);
            return;
        }
    }

    if (me->HasUnitState(UNIT_STATE_CASTING))
        return;

    spellEvents.Update(diff);

    if (!UpdateVictim())
        return;

    if (uint32 spellId = spellEvents.ExecuteEvent())
    {
        DoCast(spellId);
        spellEvents.ScheduleEvent(spellId, AISpellInfo[spellId].cooldown + rand32() % AISpellInfo[spellId].cooldown);
    }
    else
        DoMeleeAttackIfReady();
}

bool CombatAI::UpdateVictim()
{
    if (!me->HasReactState(REACT_PASSIVE))
    {
        if (Unit* victim = me->SelectVictim(false))
        {
            if (!me->IsFocusing(nullptr, true))
                AttackStart(victim);
        }
        else if (me->IsInCombat())
            EnterEvadeMode(CreatureAI::EVADE_REASON_NO_HOSTILES);

        return me->GetVictim() != nullptr;
    }
    else if (me->getThreatManager().isThreatListEmpty())
    {
        EnterEvadeMode(EVADE_REASON_NO_HOSTILES);
        return false;
    }

    return true;
}

void CombatAI::SpellInterrupted(uint32 spellId, uint32 unTimeMs)
{
    spellEvents.RescheduleEvent(spellId, unTimeMs);
}

void CombatAI::MoveCombat(Position destination)
{
    me->GetMotionMaster()->MovePoint(POINT_ID_COMBAT_MOVEMENT, destination);
    combatMoveDest = destination;
}

void CombatAI::MovementInform(uint32 type, uint32 id)
{
    if (type == POINT_MOTION_TYPE && id == POINT_ID_COMBAT_MOVEMENT)
        combatMoveDest.reset();
}

void CombatAI::EnterEvadeMode(EvadeReason why)
{
    if (!_EnterEvadeMode(why))
        return;

    if (combatMoveDest.is_initialized())
        MoveCombat(*combatMoveDest);
    else
    {
        if (!me->GetVehicle()) // otherwise me will be in evade mode forever
        {
            if (Unit* owner = me->GetCharmerOrOwner())
            {
                me->GetMotionMaster()->Clear(false);
                me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, me->GetFollowAngle(), MOTION_SLOT_ACTIVE);
            }
            else
            {
                // Required to prevent attacking creatures that are evading and cause them to reenter combat
                // Does not apply to MoveFollow
                me->AddUnitState(UNIT_STATE_EVADE);
                me->GetMotionMaster()->MoveTargetedHome();
            }
        }
    }
}

/////////////////
// CasterAI
/////////////////

void CasterAI::InitializeAI()
{
    CombatAI::InitializeAI();

    m_attackDist = 30.0f;
    for (SpellVct::iterator itr = spells.begin(); itr != spells.end(); ++itr)
        if (AISpellInfo[*itr].condition == AICOND_COMBAT && m_attackDist > GetAISpellInfo(*itr)->maxRange)
            m_attackDist = GetAISpellInfo(*itr)->maxRange;
    if (m_attackDist == 30.0f)
        m_attackDist = MELEE_RANGE;
}

void CasterAI::EnterCombat(Unit* who)
{
    if (spells.empty())
        return;

    uint32 spell = rand32() % spells.size();
    uint32 count = 0;
    for (SpellVct::iterator itr = spells.begin(); itr != spells.end(); ++itr, ++count)
    {
        if (AISpellInfo[*itr].condition == AICOND_AGGRO)
            me->CastSpell(who, *itr, false);
        else if (AISpellInfo[*itr].condition == AICOND_COMBAT)
        {
            uint32 cooldown = GetAISpellInfo(*itr)->realCooldown;
            if (count == spell)
            {
                DoCast(spells[spell]);
                cooldown += me->GetCurrentSpellCastTime(*itr);
            }
            events.ScheduleEvent(*itr, cooldown);
        }
    }
}

void CasterAI::UpdateAI(uint32 diff)
{
    if (!UpdateVictim())
        return;

    events.Update(diff);

    if (me->GetVictim() && me->EnsureVictim()->HasBreakableByDamageCrowdControlAura(me))
    {
        me->InterruptNonMeleeSpells(false);
        return;
    }

    if (me->HasUnitState(UNIT_STATE_CASTING))
        return;

    if (uint32 spellId = events.ExecuteEvent())
    {
        DoCast(spellId);
        uint32 casttime = me->GetCurrentSpellCastTime(spellId);
        events.ScheduleEvent(spellId, (casttime ? casttime : 500) + GetAISpellInfo(spellId)->realCooldown);
    }
}

//////////////
// ArcherAI
//////////////

ArcherAI::ArcherAI(Creature* c) : CreatureAI(c)
{
    if (!me->m_spells[0])
        TC_LOG_ERROR("misc", "ArcherAI set for creature (entry = %u) with spell1=0. AI will do nothing", me->GetEntry());

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(me->m_spells[0]);
    m_minRange = spellInfo ? spellInfo->GetMinRange(false) : 0;

    if (!m_minRange)
        m_minRange = MELEE_RANGE;
    me->m_CombatDistance = spellInfo ? spellInfo->GetMaxRange(false) : 0;
    me->m_SightDistance = me->m_CombatDistance;
}

void ArcherAI::AttackStart(Unit* who)
{
    if (!who)
        return;

    if (me->IsWithinCombatRange(who, m_minRange))
    {
        if (me->Attack(who, true) && !who->IsFlying())
            me->GetMotionMaster()->MoveChase(who);
    }
    else
    {
        if (me->Attack(who, false) && !who->IsFlying())
            me->GetMotionMaster()->MoveChase(who, me->m_CombatDistance);
    }

    if (who->IsFlying())
        me->GetMotionMaster()->MoveIdle();
}

void ArcherAI::UpdateAI(uint32 /*diff*/)
{
    if (!UpdateVictim())
        return;

    if (!me->IsWithinCombatRange(me->GetVictim(), m_minRange))
        DoSpellAttackIfReady(me->m_spells[0]);
    else
        DoMeleeAttackIfReady();
}

//////////////
// TurretAI
//////////////

TurretAI::TurretAI(Creature* c) : CreatureAI(c)
{
    if (!me->m_spells[0])
        TC_LOG_ERROR("misc", "TurretAI set for creature (entry = %u) with spell1=0. AI will do nothing", me->GetEntry());

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(me->m_spells[0]);
    m_minRange = spellInfo ? spellInfo->GetMinRange(false) : 0;
    me->m_CombatDistance = spellInfo ? spellInfo->GetMaxRange(false) : 0;
    me->m_SightDistance = me->m_CombatDistance;
}

bool TurretAI::CanAIAttack(const Unit* /*who*/) const
{
    /// @todo use one function to replace it
    if (!me->IsWithinCombatRange(me->GetVictim(), me->m_CombatDistance)
        || (m_minRange && me->IsWithinCombatRange(me->GetVictim(), m_minRange)))
        return false;
    return true;
}

void TurretAI::AttackStart(Unit* who)
{
    if (who)
        me->Attack(who, false);
}

void TurretAI::UpdateAI(uint32 /*diff*/)
{
    if (!UpdateVictim())
        return;

    DoSpellAttackIfReady(me->m_spells[0]);
}

//////////////
// VehicleAI
//////////////

VehicleAI::VehicleAI(Creature* creature) : CreatureAI(creature), m_HasConditions(false), m_ConditionsTimer(VEHICLE_CONDITION_CHECK_TIME)
{
    LoadConditions();
    m_DoDismiss = false;
    m_DismissTimer = VEHICLE_DISMISS_TIME;
}

// NOTE: VehicleAI::UpdateAI runs even while the vehicle is mounted
void VehicleAI::UpdateAI(uint32 diff)
{
    CheckConditions(diff);

    if (m_DoDismiss)
    {
        if (m_DismissTimer < diff)
        {
            m_DoDismiss = false;
            me->DespawnOrUnsummon();
        }
        else
            m_DismissTimer -= diff;
    }
}

void VehicleAI::OnCharmed(bool apply)
{
    if (!me->GetVehicleKit()->IsVehicleInUse() && !apply && m_HasConditions) // was used and has conditions
    {
        m_DoDismiss = true; // needs reset
    }
    else if (apply)
        m_DoDismiss = false; // in use again

    m_DismissTimer = VEHICLE_DISMISS_TIME; // reset timer
}

void VehicleAI::LoadConditions()
{
    m_HasConditions = sConditionMgr->HasConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_CREATURE_TEMPLATE_VEHICLE, me->GetEntry());
}

void VehicleAI::CheckConditions(uint32 diff)
{
    if (m_ConditionsTimer < diff)
    {
        if (m_HasConditions)
        {
            if (Vehicle* vehicleKit = me->GetVehicleKit())
                for (SeatMap::iterator itr = vehicleKit->Seats.begin(); itr != vehicleKit->Seats.end(); ++itr)
                    if (Unit* passenger = ObjectAccessor::GetUnit(*me, itr->second.Passenger.Guid))
                    {
                        if (Player* player = passenger->ToPlayer())
                        {
                            if (!sConditionMgr->IsObjectMeetingNotGroupedConditions(CONDITION_SOURCE_TYPE_CREATURE_TEMPLATE_VEHICLE, me->GetEntry(), player, me))
                            {
                                player->ExitVehicle();
                                return; // check other pessanger in next tick
                            }
                        }
                    }
        }
        m_ConditionsTimer = VEHICLE_CONDITION_CHECK_TIME;
    }
    else
        m_ConditionsTimer -= diff;
}
