/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2020-2022,  Chongqing University of Posts and TelecommunicationsK
 
 *
 * This file is my extension of NFD (Named Data Networking Forwarding Daemon).
 * Jiangtao Luo
 * 16 Mar 2020

 */

#include "randomwait-strategy.hpp"
#include "algorithm.hpp"
#include "core/logger.hpp"

#include "core/scheduler.hpp"
#include "core/random.hpp"

namespace nfd {
namespace fw {

NFD_REGISTER_STRATEGY(RandomWaitStrategy);

NFD_LOG_INIT(RandomWaitStrategy);

const time::milliseconds RandomWaitStrategy::RETX_SUPPRESSION_INITIAL(10);
const time::milliseconds RandomWaitStrategy::RETX_SUPPRESSION_MAX(250);

const time::microseconds RandomWaitStrategy::DELAY_MAX(3000); // 3 ms
const time::microseconds RandomWaitStrategy::DELAY_MIN(500); // 0.5 ms
  
const time::milliseconds RandomWaitStrategy::RETX_TIMER_UNIT(500); // 500ms per retx

  const uint32_t MAX_RETX_COUNT = 5; // maximum allowed retransmission

RandomWaitStrategy::RandomWaitStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder)
      , ProcessNackTraits(this)
  , m_retxSuppression(RETX_SUPPRESSION_INITIAL,
                      RetxSuppressionExponential::DEFAULT_MULTIPLIER,
                      RETX_SUPPRESSION_MAX)
 {
  ParsedInstanceName parsed = parseInstanceName(name);
  if (!parsed.parameters.empty()) {
    BOOST_THROW_EXCEPTION(std::invalid_argument("RandomWaitStrategy does not accept parameters"));
  }
  if (parsed.version && *parsed.version != getStrategyName()[-1].toVersion()) {
    BOOST_THROW_EXCEPTION(std::invalid_argument(
      "RandomWaitStrategy does not support version " + to_string(*parsed.version)));
  }
  this->setInstanceName(makeInstanceName(name, getStrategyName()));

}

const Name&
RandomWaitStrategy::getStrategyName()
{
  static Name strategyName("/localhost/nfd/strategy/random-wait/%FD%03");
  return strategyName;
}

void
RandomWaitStrategy::afterReceiveInterest(const Face& inFace, const Interest& interest,
                                        const shared_ptr<pit::Entry>& pitEntry)
{
 // Jiangtao Luo. 14 Feb 2020
  NFD_LOG_DEBUG("RandomWait slected for :"<<interest.getName() );
 
  // // Clone a new Interest. Jiangtao Luo. 18 Mar 2020
  // Block block = interest.wireEncode();
  // shared_ptr<Interest> pNewInterest = make_shared<Interest>(block);
  
  // int hopCount = interest.getHopCount();
  // pNewInterest->setHopCount(++hopCount); // increase hop count
  
  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();

  int nEligibleNextHops = 0;

  bool isSuppressed = false;

  for (const auto& nexthop : nexthops) {
    Face& outFace = nexthop.getFace();

    RetxSuppressionResult suppressResult = m_retxSuppression.decidePerUpstream(*pitEntry, outFace);

    if (suppressResult == RetxSuppressionResult::SUPPRESS) {
      NFD_LOG_DEBUG(interest << " from=" << inFace.getId()
                  << "to=" << outFace.getId() << " suppressed");
      isSuppressed = true;
      continue;
    }

    if ((outFace.getId() == inFace.getId() && outFace.getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) ||
        wouldViolateScope(inFace, interest, outFace)) {
      continue;
    }

    if (inFace.getScope() ==  ndn::nfd::FACE_SCOPE_LOCAL || // from local
        outFace.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {  // out to local

      NFD_LOG_DEBUG(interest << " From/To local, send now. from=" << inFace.getId()
                           << " to=" << outFace.getId());
      
      this->sendInterest(pitEntry, outFace, interest);
      
      return;
    }

    // Receive from ad-hoc link
    //Send the new Interest with increased hopcount and random delay. Jiangtao Luo. 18 March 2020
    NFD_LOG_DEBUG(interest << " Out from=" << inFace.getId()
                           << " pitEntry-to=" << outFace.getId());

    this->sendInterestLater(outFace, interest, pitEntry);
    
    if (suppressResult == RetxSuppressionResult::FORWARD) {
      m_retxSuppression.incrementIntervalForOutRecord(*pitEntry->getOutRecord(outFace));
    }
    ++nEligibleNextHops;
  }

  if (nEligibleNextHops == 0 && !isSuppressed) {
    NFD_LOG_DEBUG(interest << " from=" << inFace.getId() << " noNextHop");

    lp::NackHeader nackHeader;
    nackHeader.setReason(lp::NackReason::NO_ROUTE);
    this->sendNack(pitEntry, inFace, nackHeader);

    this->rejectPendingInterest(pitEntry);

   }
}

void
RandomWaitStrategy::afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                                    const shared_ptr<pit::Entry>& pitEntry)
{
  this->processNack(inFace, nack, pitEntry);
}

void
RandomWaitStrategy::sendInterestLater(Face& outFace, const Interest& interest,
                                      const shared_ptr<pit::Entry>& pitEntry)
{
  // 1ms -10ms
  std::uniform_int_distribution <uint64_t> dist( DELAY_MIN.count(), DELAY_MAX.count());
  time::microseconds delay = time::microseconds(dist(getGlobalRng()));

  NFD_LOG_DEBUG("RandomWaitStrategy::sendInterestLater for "
                << interest.getName().toUri()
                << " in " << delay);

  //getForwarder().setRelayTimerForInterest(pitEntry, delay, outFace, interest);
  getForwarder().setRelayTimerForInterest(delay, outFace.getId(), interest);
}

void
RandomWaitStrategy::afterSendInterest(const shared_ptr<pit::Entry>& pitEntry, Face& outFace,
                                      const Interest& interest)
{
  // if allowed, schedule retransmission
  if (pitEntry->retxCount < MAX_RETX_COUNT) {
    time::milliseconds delay =
      time::milliseconds((++pitEntry->retxCount)*RETX_TIMER_UNIT);

    //getForwarder().setRetxTimerForInterest(pitEntry, delay, outFace, interest);
    getForwarder().setRetxTimerForInterest(delay, outFace.getId(), interest);
    
    
  }
  else
    {
      this->onDroppedInterest(outFace, interest);
    }
}

void
RandomWaitStrategy::afterReceiveData(const shared_ptr<pit::Entry>& pitEntry,
                           const Face& inFace, const Data& data)
{
  NFD_LOG_DEBUG("afterReceiveData pitEntry=" << pitEntry->getName() <<
                " inFace=" << inFace.getId() << " data=" << data.getName());

  this->beforeSatisfyInterest(pitEntry, inFace, data);

  this->sendDataToAll(pitEntry, inFace, data);
}

void
RandomWaitStrategy::sendDataToAll(const shared_ptr<pit::Entry>& pitEntry,
                                  const Face& inFace, const Data& data)
{
  std::set<Face*> pendingDownstreams;
  auto now = time::steady_clock::now();

  // remember pending downstreams
  for (const pit::InRecord& inRecord : pitEntry->getInRecords()) {
    if (inRecord.getExpiry() > now) {
      if (inRecord.getFace().getId() == inFace.getId() &&
          inRecord.getFace().getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) {
        continue;
      }
      pendingDownstreams.insert(&inRecord.getFace());
    }
  }

  for (const Face* pendingDownstream : pendingDownstreams) {

    if (inFace.getScope() ==  ndn::nfd::FACE_SCOPE_LOCAL ||  // from local
        pendingDownstream->getScope() == ndn::nfd::FACE_SCOPE_LOCAL ||  // to local
        pendingDownstream->getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) { // non ad-hoc
     NFD_LOG_DEBUG("From/to local or non ad-hoc link: send now ...");
      this->sendData(pitEntry, data, *pendingDownstream);      
    }
    else { // ad-hoc relay
      NFD_LOG_DEBUG("ad-hoc link relay: random wait ...");
      this->sendDataLater(*pendingDownstream, data);
    }
    
  }
}

void
RandomWaitStrategy::sendDataLater(const Face& outFace, const Data& data)
{
  // 1ms -10ms
  std::uniform_int_distribution <uint64_t> dist( DELAY_MIN.count(), DELAY_MAX.count());
  time::microseconds delay = time::microseconds(dist(getGlobalRng()));

  NFD_LOG_DEBUG("sendDataLater for data="
                << data.getName() << " to Face = " << outFace.getId() 
                << " after " << delay);

  getForwarder().setRelayTimerForData(delay, outFace.getId(), data);
}
void
RandomWaitStrategy::afterContentStoreHit(const shared_ptr<pit::Entry>& pitEntry,
                        const Face& outFace, const Data& data)
{
  NFD_LOG_DEBUG("afterContentStoreHit pitEntry=" << pitEntry->getName() <<
                " outFace=" << outFace.getId() << " data=" << data.getName());
  // isExpiredToRelayData
  cs::Entry *csEntry = getForwarder().getCs().findEntry(data.getName());
  if (!csEntry->isExpiredToRelayData()) { // if not expired, cancel scheduled tx
    NFD_LOG_DEBUG("Cancel scheduled Data relay and send now!!!");
    scheduler::cancel(csEntry->relayTimerForData);
  }
  this->sendData(pitEntry, data, outFace);
    
}

} // namespace fw
} // namespace nfd
