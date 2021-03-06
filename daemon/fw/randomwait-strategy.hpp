/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2020-2022,  Chongqing University of Posts and TelecommunicationsK
 
 * For random-wait strategy
 * This file is my extension of NFD (Named Data Networking Forwarding Daemon).
 * Jiangtao Luo
 * 16 Mar 2020

 */

#ifndef NFD_DAEMON_FW_RANDOMWAIT_STRATEGY_HPP
#define NFD_DAEMON_FW_RANDOMWAIT_STRATEGY_HPP

#include "strategy.hpp"
#include "process-nack-traits.hpp"
#include "retx-suppression-exponential.hpp"

//#include "scheduler.hpp"




namespace nfd {
namespace fw {

/** \brief a forwarding strategy that forwards Interest to all FIB nexthops
 */
class RandomWaitStrategy : public Strategy
                         , public ProcessNackTraits<RandomWaitStrategy>
{
public:

  explicit
  RandomWaitStrategy(Forwarder& forwarder, const Name& name = getStrategyName());

  static const Name&
  getStrategyName();

  void
  afterReceiveInterest(const Face& inFace, const Interest& interest,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  void
  afterReceiveNack(const Face& inFace, const lp::Nack& nack,
                   const shared_ptr<pit::Entry>& pitEntry) override;

  void
  afterSendInterest(const shared_ptr<pit::Entry>& pitEntry,
                    Face& outFace, const Interest& interest);

  void
  afterReceiveData(const shared_ptr<pit::Entry>& pitEntry,
                   const Face& inFace, const Data& data);

  void
   afterContentStoreHit(const shared_ptr<pit::Entry>& pitEntry,
                        const Face& outFace, const Data& data);
protected:
  // Clone a new Interest, increase ite hop couont and send it in a random delay
   VIRTUAL_WITH_TESTS void
   sendInterestLater(Face& outFace, const Interest& interest, const shared_ptr<pit::Entry>& pitEntry);
  //sendInterestLater(Face& outFace, const Interest& interest);

   // Scheduled send
   //VIRTUAL_WITH_TESTS void
  //doSendInterestLater(Face& outFace, const Interest& interest);
  VIRTUAL_WITH_TESTS void
  sendDataToAll(const shared_ptr<pit::Entry>& pitEntry, const Face& inFace, const Data& data);

  VIRTUAL_WITH_TESTS void
  sendDataLater(const Face& outFace, const Data& data);

private:
  friend ProcessNackTraits<RandomWaitStrategy>;
  RetxSuppressionExponential m_retxSuppression;

PUBLIC_WITH_TESTS_ELSE_PRIVATE:
  static const time::milliseconds RETX_SUPPRESSION_INITIAL;
  static const time::milliseconds RETX_SUPPRESSION_MAX;

  static const time::microseconds DELAY_MAX_INTEREST; // maximum delay for Interest
  static const time::microseconds DELAY_MIN_INTEREST; // minimum delay for Interest

  static const time::milliseconds RETX_TIMER_UNIT; // each extra delay per retx

  static const time::microseconds DELAY_MAX_DATA; // maximum dealy for Data
  static const time::microseconds DELAY_MIN_DATA; // minimum delay for Data

  //EventId m_sendInterest; // EventId of sending Interest
};
 
} // namespace fw
} // namespace nfd

#endif // NFD_DAEMON_FW_RANDOMWAIT_STRATEGY_HPP
