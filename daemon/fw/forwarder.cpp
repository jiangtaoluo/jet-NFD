/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2019,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "forwarder.hpp"

#include "algorithm.hpp"
#include "best-route-strategy2.hpp"
#include "strategy.hpp"
#include "core/logger.hpp"
#include "table/cleanup.hpp"

#include <ndn-cxx/lp/tags.hpp>

#include "face/null-face.hpp"

namespace nfd {

NFD_LOG_INIT(Forwarder);

static Name
getDefaultStrategyName()
{
  return fw::BestRouteStrategy2::getStrategyName();
}

Forwarder::Forwarder()
  : m_unsolicitedDataPolicy(new fw::DefaultUnsolicitedDataPolicy())
  , m_fib(m_nameTree)
  , m_pit(m_nameTree)
  , m_measurements(m_nameTree)
  , m_strategyChoice(*this)
  , m_csFace(face::makeNullFace(FaceUri("contentstore://")))
{
  getFaceTable().addReserved(m_csFace, face::FACEID_CONTENT_STORE);

  m_faceTable.afterAdd.connect([this] (Face& face) {
    face.afterReceiveInterest.connect(
      [this, &face] (const Interest& interest) {
        this->startProcessInterest(face, interest);
      });
    face.afterReceiveData.connect(
      [this, &face] (const Data& data) {
        this->startProcessData(face, data);
      });
    face.afterReceiveNack.connect(
      [this, &face] (const lp::Nack& nack) {
        this->startProcessNack(face, nack);
      });
    face.onDroppedInterest.connect(
      [this, &face] (const Interest& interest) {
        this->onDroppedInterest(face, interest);
      });
  });

  m_faceTable.beforeRemove.connect([this] (Face& face) {
    cleanupOnFaceRemoval(m_nameTree, m_fib, m_pit, face);
  });

  m_strategyChoice.setDefaultStrategy(getDefaultStrategyName());
}

Forwarder::~Forwarder() = default;


////////////////////////////////
// Jiangtao Luo. 18 Mar 2020
void Forwarder::onRandomWaitLoopInterest(Face& inFace, const Interest& interest)
{
  NFD_LOG_DEBUG("Entering onRandomWaitLoopInterest: ...");
  // Cancel the scheduled transmission
   // Jiangtao Luo. 21 Mar 2020
  shared_ptr<pit::Entry> pitEntry = m_pit.find(interest);

  if (pitEntry != nullptr && !pitEntry->isExpiredToSendInterest()) {
    NFD_LOG_DEBUG("Cancel the scheduled Interest transmission!");
    scheduler::cancel(pitEntry->relayTimerForInterest);
  }
  // else if (!pitEntry->isExpiredRtxInterest()) {  // if re-tx not expired, cancel it.
  //   NFD_LOG_DEBUG("Cancel the scheduled Interest re-transmission!");

  //   pitEntry->retxCount = 0; // reset re-tx count
  //   scheduler::cancel(pitEntry->retxTimerForInterest);
  // }
  else if (pitEntry == nullptr) {
    NFD_LOG_DEBUG("PIT entry expired! Drop loop interest!");
  }
  else {
     NFD_LOG_DEBUG("Drop loop interest!");
  }

}

  ////////////////////////////////
  // Jiangtao Luo. 12 Feb
void Forwarder::onDataEmergency(Face& inFace, const Data& data)
{
  NFD_LOG_INFO("onDataEmergency: " << data.getName() <<
                " Nonce: " << data.getNonce());

  // detect duplicate Nonce
  bool bDuplicate = m_dataNonceList.has(data.getName(),
                                        data.getNonce());
  NFD_LOG_DEBUG("Data Nonce List size: "<< m_dataNonceList.size());
  if(bDuplicate) {
    NFD_LOG_DEBUG("Duplicate Data Nonce found: "<< data.getNonce()
                  << ", Dropped!");
    return;
  }
  else {
    m_dataNonceList.add(data.getName(), data.getNonce());
  }
    // 
  
  // foreach pending downstream, all in m_faceTable
  // FaceMap = std::map< FaceId, shared_ptr< Face > >
  //std::map<FaceId, shared_ptr<Face> >::const_iterator iter;
  FaceTable::const_iterator iter;
  for (iter = m_faceTable.begin(); iter != m_faceTable.end(); ++iter) {
    NFD_LOG_DEBUG("LinkType: " << iter->getLinkType());

    // All output
    // this->onOutgoingData(data, *iter);
    
    if ( iter->getId() != inFace.getId() ||
         iter->getLinkType() == ndn::nfd::LINK_TYPE_AD_HOC){
       this->onOutgoingData(data, *iter);
     }
     continue;
  }
  
}
    
  ////////////////////////////////

void
Forwarder::onIncomingInterest(Face& inFace, const Interest& interest)
{
  // receive Interest
  NFD_LOG_DEBUG(this <<"->onIncomingInterest face=" << inFace.getId() <<
                " interest=" << interest.getName() << "nonce=" <<
                interest.getNonce()); // add nonce. Jiangtao Luo
  interest.setTag(make_shared<lp::IncomingFaceIdTag>(inFace.getId()));
  ++m_counters.nInInterests;



   // /localhost scope control
  bool isViolatingLocalhost = inFace.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(interest.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onIncomingInterest face=" << inFace.getId() <<
                  " interest=" << interest.getName() << " violates /localhost");
    // (drop)
    return;
  }

  // detect duplicate Nonce with Dead Nonce List
  bool hasDuplicateNonceInDnl = m_deadNonceList.has(interest.getName(), interest.getNonce());
  if (hasDuplicateNonceInDnl) {
    // goto Interest loop pipeline
    this->onInterestLoop(inFace, interest);
    return;
  }

  // strip forwarding hint if Interest has reached producer region
  if (!interest.getForwardingHint().empty() &&
      m_networkRegionTable.isInProducerRegion(interest.getForwardingHint())) {
    NFD_LOG_DEBUG("onIncomingInterest face=" << inFace.getId() <<
                  " interest=" << interest.getName() << " reaching-producer-region");
    const_cast<Interest&>(interest).setForwardingHint({});
  }

  // PIT insert
  shared_ptr<pit::Entry> pitEntry = m_pit.insert(interest).first;

  // Jiangtao LUo. 14 Feb 2020
  NFD_LOG_DEBUG("PIT inserted for : " << pitEntry->getName());

  // detect duplicate Nonce in PIT entry
  int dnw = fw::findDuplicateNonce(*pitEntry, interest.getNonce(), inFace);
  bool hasDuplicateNonceInPit = dnw != fw::DUPLICATE_NONCE_NONE;
  if (inFace.getLinkType() == ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    // for p2p face: duplicate Nonce from same incoming face is not loop
    hasDuplicateNonceInPit = hasDuplicateNonceInPit && !(dnw & fw::DUPLICATE_NONCE_IN_SAME);
  }
  if (hasDuplicateNonceInPit) {
    // goto Interest loop pipeline
    this->onInterestLoop(inFace, interest);
    return;
  }

  // is pending?
  if (!pitEntry->hasInRecords()) {
    if (m_csFromNdnSim == nullptr) {
      m_cs.find(interest,
                bind(&Forwarder::onContentStoreHit, this, std::ref(inFace), pitEntry, _1, _2),
                bind(&Forwarder::onContentStoreMiss, this, std::ref(inFace), pitEntry, _1));
    }
    else {
      shared_ptr<Data> match = m_csFromNdnSim->Lookup(interest.shared_from_this());
      if (match != nullptr) {
        this->onContentStoreHit(inFace, pitEntry, interest, *match);
      }
      else {
        this->onContentStoreMiss(inFace, pitEntry, interest);
      }
    }
  }
  else {
    ////////////////////////////////
    // has in-records but not loop: different nonce
    // Jiangtao Luo. 23 Mar 2020
    if (!pitEntry->isExpiredToSendInterest()) {
      NFD_LOG_INFO("Cancel the scheduled Interest transmission (old nonce)!");
      scheduler::cancel(pitEntry->relayTimerForInterest);
    }
   if (!pitEntry->isExpiredRtxInterest()) {  // if re-tx not expired, cancel it.
      NFD_LOG_INFO("Cancel the scheduled Interest re-transmission (old nonce)!)");
      
      pitEntry->retxCount = 0; // reset re-tx count
      scheduler::cancel(pitEntry->retxTimerForInterest);
    }
   ////////////////////////////////
    this->onContentStoreMiss(inFace, pitEntry, interest);
  }
}

void
Forwarder::onInterestLoop(Face& inFace, const Interest& interest)
{
  // if multi-access or ad hoc face, drop
  if (inFace.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    // NFD_LOG_DEBUG("onInterestLoop face=" << inFace.getId() <<
    //               " interest=" << interest.getName() <<
    //               " drop");
    NFD_LOG_DEBUG("onInterestLoop face=" << inFace.getId() <<
                  " interest=" << interest.getName());
    ////////////////////////////////
    // reditect for randomWaitStrategy
    // Jiangtao Luo. 18 Mar 2020
    //fw::Strategy s = m_strategyChoice.findEffectiveStrategy(interest.getName());
    std::string strName = m_strategyChoice.findEffectiveStrategy(interest.getName()).getInstanceName().toUri();

    if (strName.find("random-wait") != std::string::npos) {
      // randomWaitStrategy
      return this->onRandomWaitLoopInterest(inFace, interest);
    }
    
  }

  NFD_LOG_DEBUG("onInterestLoop face=" << inFace.getId() <<
                " interest=" << interest.getName() <<
                " send-Nack-duplicate");

  // send Nack with reason=DUPLICATE
  // note: Don't enter outgoing Nack pipeline because it needs an in-record.
  lp::Nack nack(interest);
  nack.setReason(lp::NackReason::DUPLICATE);
  inFace.sendNack(nack);
}

static inline bool
compare_InRecord_expiry(const pit::InRecord& a, const pit::InRecord& b)
{
  return a.getExpiry() < b.getExpiry();
}

void
Forwarder::onContentStoreMiss(const Face& inFace, const shared_ptr<pit::Entry>& pitEntry,
                              const Interest& interest)
{
  NFD_LOG_DEBUG("onContentStoreMiss interest=" << interest.getName());
  ++m_counters.nCsMisses;

  // insert in-record
  pitEntry->insertOrUpdateInRecord(const_cast<Face&>(inFace), interest);

  // set PIT expiry timer to the time that the last PIT in-record expires
  auto lastExpiring = std::max_element(pitEntry->in_begin(), pitEntry->in_end(), &compare_InRecord_expiry);
  auto lastExpiryFromNow = lastExpiring->getExpiry() - time::steady_clock::now();
  this->setExpiryTimer(pitEntry, time::duration_cast<time::milliseconds>(lastExpiryFromNow));

  // has NextHopFaceId?
  shared_ptr<lp::NextHopFaceIdTag> nextHopTag = interest.getTag<lp::NextHopFaceIdTag>();
  if (nextHopTag != nullptr) {
    // chosen NextHop face exists?
    Face* nextHopFace = m_faceTable.get(*nextHopTag);
    if (nextHopFace != nullptr) {
      NFD_LOG_DEBUG("onContentStoreMiss interest=" << interest.getName() << " nexthop-faceid=" << nextHopFace->getId());
      // go to outgoing Interest pipeline
      // scope control is unnecessary, because privileged app explicitly wants to forward
      this->onOutgoingInterest(pitEntry, *nextHopFace, interest);
    }
    return;
  }

  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.afterReceiveInterest(inFace, interest, pitEntry); });
}

void
Forwarder::onContentStoreHit(const Face& inFace, const shared_ptr<pit::Entry>& pitEntry,
                             const Interest& interest, const Data& data)
{
  NFD_LOG_DEBUG("onContentStoreHit interest=" << interest.getName());
  ++m_counters.nCsHits;

  data.setTag(make_shared<lp::IncomingFaceIdTag>(face::FACEID_CONTENT_STORE));
  // XXX should we lookup PIT for other Interests that also match csMatch?

  pitEntry->isSatisfied = true;
  pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

  // set PIT expiry timer to now
  this->setExpiryTimer(pitEntry, 0_ms);

  beforeSatisfyInterest(*pitEntry, *m_csFace, data);
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.beforeSatisfyInterest(pitEntry, *m_csFace, data); });

  // dispatch to strategy: after Content Store hit
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.afterContentStoreHit(pitEntry, inFace, data); });
}

void
Forwarder::onOutgoingInterest(const shared_ptr<pit::Entry>& pitEntry, Face& outFace, const Interest& interest)
{
  BOOST_ASSERT(pitEntry); // Jiangtao Luo. 26 Mar 2020
  NFD_LOG_DEBUG(this <<"->onOutgoingInterest face=" << outFace.getId() <<
                " interest=" << pitEntry->getName() << " Nonce= "<< interest.getNonce()); // add nonce. Jiangtao Luo

  // insert out-record
  pitEntry->insertOrUpdateOutRecord(outFace, interest);

  ////////////////////////////////
    // Jiangtao Luo. 1 April 2020
  interest.setTag(make_shared<lp::CchTag>(1));
  // if (outFace.getId() == 257) {
  //   NFD_LOG_INFO("FaceId = 257, set on CCH!!!");
  //   outFace.setInterestOnCch(); // set Interest on CCH
  // }
  ////////////////////////////////
  // send Interest
  outFace.sendInterest(interest);
  ++m_counters.nOutInterests;

  ////////////////////////////////////////////////////////////////
  // if random wait, start re-tx
  // Jiangtao Luo. 22 March
  // if not from local

  std::list<nfd::pit::InRecord>::iterator inBegin =  pitEntry->in_begin();

  nfd::pit::InRecord& inRecord = *inBegin;
  
  Face& inFace = inRecord.getFace();
  
  if (inFace.getScope() != ndn::nfd::FACE_SCOPE_LOCAL &&
      outFace.getScope() != ndn::nfd::FACE_SCOPE_LOCAL)
    { // not from local and not to local
    NFD_LOG_DEBUG("inFace="<<inFace.getId() << ", Not from local, schedule for re-transmission ...");
    std::string strName =
      m_strategyChoice.findEffectiveStrategy(interest.getName()).getInstanceName().toUri();

    if (strName.find("random-wait") != std::string::npos) {
      NFD_LOG_DEBUG("Dispatch to RandomWait afterSendInterest: re-tx counter="
                    << pitEntry->retxCount);
      this->dispatchToStrategy(*pitEntry,
                               [&] (fw::Strategy& strategy) {
                                 strategy.afterSendInterest(pitEntry, outFace, interest); });
    }
  }
  ////////////////////////////////////////////////////////////////

}

void
Forwarder::onInterestFinalize(const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("onInterestFinalize interest=" << pitEntry->getName() <<
                (pitEntry->isSatisfied ? " satisfied" : " unsatisfied")
                <<" nonce = " << pitEntry->getInterest().getNonce()); // add nonce. Jiangtao Luo.

  if (!pitEntry->isSatisfied) {
    beforeExpirePendingInterest(*pitEntry);
  }

  // Dead Nonce List insert if necessary
  this->insertDeadNonceList(*pitEntry, 0);

  // Increment satisfied/unsatisfied Interests counter
  if (pitEntry->isSatisfied) {
    ++m_counters.nSatisfiedInterests;
  }
  else {
    ++m_counters.nUnsatisfiedInterests;
  }

 ////////////////////////////////

   // Check before cancel scheduled interest relay or retransmission
  // Jiangtao Luo. 25 Mar 2020
  
  if (!pitEntry->isExpiredToSendInterest()) {
    NFD_LOG_DEBUG("Cancel the scheduled Interest transmission!");
    scheduler::cancel(pitEntry->relayTimerForInterest);
  }
  if (!pitEntry->isExpiredRtxInterest()) {  // if re-tx not expired, cancel it.
    NFD_LOG_DEBUG("Cancel the scheduled Interest re-transmission!");

    pitEntry->retxCount = 0; // reset re-tx count
    scheduler::cancel(pitEntry->retxTimerForInterest);
  }
 ////////////////////////////////
  
  // PIT delete
  scheduler::cancel(pitEntry->expiryTimer);
  m_pit.erase(pitEntry.get());
}

void
Forwarder::onIncomingData(Face& inFace, const Data& data)
{
  // receive Data
  NFD_LOG_DEBUG(this << "->onIncomingData face=" << inFace.getId() << " data=" << data.getName());

  // Jiangtao Luo. 12 Feb 2020
  //NFD_LOG_DEBUG("Emergency Ind = " << data.getEmergencyInd());
    
  data.setTag(make_shared<lp::IncomingFaceIdTag>(inFace.getId()));
  ++m_counters.nInData;

  // /localhost scope control
  bool isViolatingLocalhost = inFace.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(data.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onIncomingData face=" << inFace.getId() <<
                  " data=" << data.getName() << " violates /localhost");
    // (drop)
    return;
  }

  ////////////////////////////////
  /**
   * Check if the Data is emergency: if yes, goto onDataEmergency
   * Modified by Jiangtao Luo. 12 Feb 2020
   * JZQ modify 4/12/2019
   */
  if(data.getEmergencyInd() == "Emergency") {
    // goto emergency Data pipeline
    this->onDataEmergency(inFace, data);
    return;
   }
  ////////////////////////////////
  

  // PIT match
  pit::DataMatchResult pitMatches = m_pit.findAllDataMatches(data);
  if (pitMatches.size() == 0) {
    // goto Data unsolicited pipeline
    this->onDataUnsolicited(inFace, data);
    return;
  }

  shared_ptr<Data> dataCopyWithoutTag = make_shared<Data>(data);
  dataCopyWithoutTag->removeTag<lp::HopCountTag>();

  // CS insert
  if (m_csFromNdnSim == nullptr)
    m_cs.insert(*dataCopyWithoutTag);
  else
    m_csFromNdnSim->Add(dataCopyWithoutTag);

  // when only one PIT entry is matched, trigger strategy: after receive Data
  if (pitMatches.size() == 1) {
    auto& pitEntry = pitMatches.front();

    NFD_LOG_DEBUG("onIncomingData matching=" << pitEntry->getName());

    // set PIT expiry timer to now
    this->setExpiryTimer(pitEntry, 0_ms);

    beforeSatisfyInterest(*pitEntry, inFace, data);
    // trigger strategy: after receive Data
    this->dispatchToStrategy(*pitEntry,
      [&] (fw::Strategy& strategy) { strategy.afterReceiveData(pitEntry, inFace, data); });

    // mark PIT satisfied
    pitEntry->isSatisfied = true;
    pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

    // Dead Nonce List insert if necessary (for out-record of inFace)
    this->insertDeadNonceList(*pitEntry, &inFace);

    // delete PIT entry's out-record
    pitEntry->deleteOutRecord(inFace);
  }
  // when more than one PIT entry is matched, trigger strategy: before satisfy Interest,
  // and send Data to all matched out faces
  else {
    std::set<Face*> pendingDownstreams;
    auto now = time::steady_clock::now();

    for (const shared_ptr<pit::Entry>& pitEntry : pitMatches) {
      NFD_LOG_DEBUG("onIncomingData matching=" << pitEntry->getName());

      // remember pending downstreams
      for (const pit::InRecord& inRecord : pitEntry->getInRecords()) {
        if (inRecord.getExpiry() > now) {
          pendingDownstreams.insert(&inRecord.getFace());
        }
      }

      // set PIT expiry timer to now
      this->setExpiryTimer(pitEntry, 0_ms);

      // invoke PIT satisfy callback
      beforeSatisfyInterest(*pitEntry, inFace, data);
      this->dispatchToStrategy(*pitEntry,
        [&] (fw::Strategy& strategy) { strategy.beforeSatisfyInterest(pitEntry, inFace, data); });

      // mark PIT satisfied
      pitEntry->isSatisfied = true;
      pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

      // Dead Nonce List insert if necessary (for out-record of inFace)
      this->insertDeadNonceList(*pitEntry, &inFace);

      // clear PIT entry's in and out records
      pitEntry->clearInRecords();
      pitEntry->deleteOutRecord(inFace);
    }

    // foreach pending downstream
    for (Face* pendingDownstream : pendingDownstreams) {
      if (pendingDownstream->getId() == inFace.getId() &&
          pendingDownstream->getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) {
        continue;
      }
      // goto outgoing Data pipeline
      this->onOutgoingData(data, *pendingDownstream);
    }
  }
}

void
Forwarder::onDataUnsolicited(Face& inFace, const Data& data)
{
  // accept to cache?
  fw::UnsolicitedDataDecision decision = m_unsolicitedDataPolicy->decide(inFace, data);
  if (decision == fw::UnsolicitedDataDecision::CACHE) {
    // CS insert
    if (m_csFromNdnSim == nullptr)
      m_cs.insert(data, true);
    else
      m_csFromNdnSim->Add(data.shared_from_this());
  }

  ////////////////////////////////
    // reditect for randomWaitStrategy
    // Jiangtao Luo. 18 Mar 2020
   std::string strName = m_strategyChoice.findEffectiveStrategy(data.getName()).
     getInstanceName().toUri();

  if (strName.find("random-wait") != std::string::npos) {
      // randomWaitStrategy
    return this->onRandomWaitDataUnsolicited(inFace, data);
  }
  else {
    NFD_LOG_DEBUG("onDataUnsolicited face=" << inFace.getId() <<
                  " data=" << data.getName() <<
                  " decision=" << decision);
  }
  ////////////////////////////////
  // NFD_LOG_DEBUG("onDataUnsolicited face=" << inFace.getId() <<
  //                 " data=" << data.getName() <<
  //                 " decision=" << decision);
}

void
Forwarder::onOutgoingData(const Data& data, Face& outFace)
{
  if (outFace.getId() == face::INVALID_FACEID) {
    NFD_LOG_WARN("onOutgoingData face=invalid data=" << data.getName());
    return;
  }
  NFD_LOG_DEBUG(this <<"->onOutgoingData face=" << outFace.getId() << " data=" << data.getName());

  // /localhost scope control
  bool isViolatingLocalhost = outFace.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(data.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onOutgoingData face=" << outFace.getId() <<
                  " data=" << data.getName() << " violates /localhost");
    // (drop)
    return;
  }

  // TODO traffic manager

  // send Data
  outFace.sendData(data);
  ++m_counters.nOutData;
}

void
Forwarder::onIncomingNack(Face& inFace, const lp::Nack& nack)
{
  // receive Nack
  nack.setTag(make_shared<lp::IncomingFaceIdTag>(inFace.getId()));
  ++m_counters.nInNacks;

  // if multi-access or ad hoc face, drop
  if (inFace.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onIncomingNack face=" << inFace.getId() <<
                  " nack=" << nack.getInterest().getName() <<
                  "~" << nack.getReason() << " face-is-multi-access");
    return;
  }

  // PIT match
  shared_ptr<pit::Entry> pitEntry = m_pit.find(nack.getInterest());
  // if no PIT entry found, drop
  if (pitEntry == nullptr) {
    NFD_LOG_DEBUG("onIncomingNack face=" << inFace.getId() <<
                  " nack=" << nack.getInterest().getName() <<
                  "~" << nack.getReason() << " no-PIT-entry");
    return;
  }

  // has out-record?
  pit::OutRecordCollection::iterator outRecord = pitEntry->getOutRecord(inFace);
  // if no out-record found, drop
  if (outRecord == pitEntry->out_end()) {
    NFD_LOG_DEBUG("onIncomingNack face=" << inFace.getId() <<
                  " nack=" << nack.getInterest().getName() <<
                  "~" << nack.getReason() << " no-out-record");
    return;
  }

  // if out-record has different Nonce, drop
  if (nack.getInterest().getNonce() != outRecord->getLastNonce()) {
    NFD_LOG_DEBUG("onIncomingNack face=" << inFace.getId() <<
                  " nack=" << nack.getInterest().getName() <<
                  "~" << nack.getReason() << " wrong-Nonce " <<
                  nack.getInterest().getNonce() << "!=" << outRecord->getLastNonce());
    return;
  }

  NFD_LOG_DEBUG("onIncomingNack face=" << inFace.getId() <<
                " nack=" << nack.getInterest().getName() <<
                "~" << nack.getReason() << " OK");

  // record Nack on out-record
  outRecord->setIncomingNack(nack);

  // set PIT expiry timer to now when all out-record receive Nack
  if (!fw::hasPendingOutRecords(*pitEntry)) {
    this->setExpiryTimer(pitEntry, 0_ms);
  }

  // trigger strategy: after receive NACK
  this->dispatchToStrategy(*pitEntry,
    [&] (fw::Strategy& strategy) { strategy.afterReceiveNack(inFace, nack, pitEntry); });
}

void
Forwarder::onOutgoingNack(const shared_ptr<pit::Entry>& pitEntry, const Face& outFace,
                          const lp::NackHeader& nack)
{
  if (outFace.getId() == face::INVALID_FACEID) {
    NFD_LOG_WARN("onOutgoingNack face=invalid" <<
                  " nack=" << pitEntry->getInterest().getName() <<
                  "~" << nack.getReason() << " no-in-record");
    return;
  }

  // has in-record?
  pit::InRecordCollection::iterator inRecord = pitEntry->getInRecord(outFace);

  // if no in-record found, drop
  if (inRecord == pitEntry->in_end()) {
    NFD_LOG_DEBUG("onOutgoingNack face=" << outFace.getId() <<
                  " nack=" << pitEntry->getInterest().getName() <<
                  "~" << nack.getReason() << " no-in-record");
    return;
  }

  // if multi-access or ad hoc face, drop
  if (outFace.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onOutgoingNack face=" << outFace.getId() <<
                  " nack=" << pitEntry->getInterest().getName() <<
                  "~" << nack.getReason() << " face-is-multi-access");
    return;
  }

  NFD_LOG_DEBUG("onOutgoingNack face=" << outFace.getId() <<
                " nack=" << pitEntry->getInterest().getName() <<
                "~" << nack.getReason() << " OK");

  // create Nack packet with the Interest from in-record
  lp::Nack nackPkt(inRecord->getInterest());
  nackPkt.setHeader(nack);

  // erase in-record
  pitEntry->deleteInRecord(outFace);

  // send Nack on face
  const_cast<Face&>(outFace).sendNack(nackPkt);
  ++m_counters.nOutNacks;
}

void
Forwarder::onDroppedInterest(Face& outFace, const Interest& interest)
{
  m_strategyChoice.findEffectiveStrategy(interest.getName()).onDroppedInterest(outFace, interest);
}

void
Forwarder::setExpiryTimer(const shared_ptr<pit::Entry>& pitEntry, time::milliseconds duration)
{
  BOOST_ASSERT(pitEntry);
  BOOST_ASSERT(duration >= 0_ms);

  scheduler::cancel(pitEntry->expiryTimer);

  pitEntry->expiryTimer = scheduler::schedule(duration, [=] { onInterestFinalize(pitEntry); });
}

void
Forwarder::insertDeadNonceList(pit::Entry& pitEntry, Face* upstream)
{
  // need Dead Nonce List insert?
  bool needDnl = true;
  if (pitEntry.isSatisfied) {
    BOOST_ASSERT(pitEntry.dataFreshnessPeriod >= 0_ms);
    needDnl = static_cast<bool>(pitEntry.getInterest().getMustBeFresh()) &&
              pitEntry.dataFreshnessPeriod < m_deadNonceList.getLifetime();
  }

  if (!needDnl) {
    return;
  }

  // Dead Nonce List insert
  if (upstream == nullptr) {
    // insert all outgoing Nonces
    const auto& outRecords = pitEntry.getOutRecords();
    std::for_each(outRecords.begin(), outRecords.end(), [&] (const auto& outRecord) {
      m_deadNonceList.add(pitEntry.getName(), outRecord.getLastNonce());
    });
  }
  else {
    // insert outgoing Nonce of a specific face
    auto outRecord = pitEntry.getOutRecord(*upstream);
    if (outRecord != pitEntry.getOutRecords().end()) {
      m_deadNonceList.add(pitEntry.getName(), outRecord->getLastNonce());
    }
  }
}

////////////////////////////////
// Jiangtao Luo. 21 Mar 2020
void
// Forwarder::setRelayTimerForInterest(const shared_ptr<pit::Entry>& pitEntry,
//                               time::microseconds delay,
//                               Face& outFace, const Interest& interest)
Forwarder::setRelayTimerForInterest(time::microseconds delay, FaceId outFaceId, const Interest& interest)

{
  BOOST_ASSERT(delay >= 0_us);

  shared_ptr<pit::Entry> pitEntry = m_pit.find(interest);

  if(pitEntry != nullptr) {
    scheduler::cancel(pitEntry->relayTimerForInterest);

    // pitEntry->relayTimerForInterest = scheduler::schedule(delay, [&, pitEntry]
    //                                                      { onOutgoingInterest(pitEntry, outFace, interest);});

    NFD_LOG_DEBUG("Set relay for Interest=" << interest.getName() <<
                  "Nonce="<< interest.getNonce() << " after delay=" << delay);
    pitEntry->relayTimerForInterest = scheduler::schedule(delay, [=]
                                                         {
                                                           const Interest& interest = pitEntry->getInterest();
                                                           Face* outFace = getFace(outFaceId);

                                                           onOutgoingInterest(pitEntry, *outFace, interest);});

    pitEntry->expireTimeToRelayInterest = time::steady_clock::now() + delay;
  }
}
  


// Set re-transmission for relayed Interest
// void
// Forwarder::setRetxTimerForInterest(const shared_ptr<pit::Entry>& pitEntry,
//                               time::milliseconds delay,
//                                Face& outFace, const Interest& interest)
void
Forwarder::setRetxTimerForInterest(time::milliseconds delay, FaceId outFaceId, const Interest& interest)
{
  BOOST_ASSERT(delay >= 0_ms);

  shared_ptr<pit::Entry> pitEntry = m_pit.find(interest);

  if (pitEntry != nullptr) {
     scheduler::cancel(pitEntry->retxTimerForInterest);

     NFD_LOG_DEBUG("Set re-tx for Interest=" << interest.getName() <<
                  "Nonce=" << interest.getNonce() << " after delay=" << delay);

  // pitEntry->retxTimerForInterest = scheduler::schedule(delay, [&, pitEntry]
  //                                                        { onOutgoingInterest(pitEntry, outFace, interest);});
     pitEntry->retxTimerForInterest =
       scheduler::schedule(delay, [=] {  const Interest& interest = pitEntry->getInterest();
                                      Face* outFace = getFace(outFaceId);
                                      onOutgoingInterest(pitEntry, *outFace, interest);});

     pitEntry->expireTimeToRetxInterest = time::steady_clock::now() + delay;
  }
  else {
    NFD_LOG_DEBUG("PIT entry expired!!!");
  }

}


////////////////////////////////
// Set relay timer for Data
// Jiangtao Luo. 24 Mar 2020
void
//Forwarder::setRelayTimerForData(time::microseconds delay, Face& outFace, const Data& data)
Forwarder::setRelayTimerForData(time::microseconds delay, FaceId outFaceId, const Data& data)
  
{
  
  BOOST_ASSERT(delay >= 0_us);
  NFD_LOG_DEBUG("setRelayTimerForData, data="
                << data.getName() << " to Face = " << outFaceId 
                << " after " << delay);

  cs::Entry *csEntry = m_cs.findEntry(data.getName());
  if (csEntry != nullptr) {

    scheduler::cancel(csEntry->relayTimerForData);

    csEntry->relayTimerForData = scheduler::schedule(delay, [=] {
    //csEntry->relayTimerForData = scheduler::schedule(delay, [=, &data] {
                                                              NFD_LOG_DEBUG("Scheduled relay data from " << this);
                                                              const Data& data2 = csEntry->getData();
                                                              Face* outFace = getFace(outFaceId);
                                                              this->onOutgoingData(data2, *outFace);});

    csEntry->expireTimeToRelayData =  time::steady_clock::now() + delay;
  }

}

void Forwarder::onRandomWaitDataUnsolicited(Face& inFace, const Data& data)
{
  cs::Entry *csEntry = m_cs.findEntry(data.getName());
  if (csEntry != nullptr) {
    scheduler::cancel(csEntry->relayTimerForData);
  }
  
}
 
////////////////////////////////

} // namespace nfd
