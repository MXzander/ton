/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "validator-group.hpp"
#include "fabric.h"
#include "ton/ton-io.hpp"
#include "td/utils/overloaded.h"
#include "common/delay.h"
#include "ton/lite-tl.hpp"
#include "ton/ton-tl.hpp"
#include "td/utils/Random.h"
#include "collator-node.hpp"

namespace ton {

namespace validator {

void ValidatorGroup::generate_block_candidate(
    td::uint32 round_id, td::Promise<validatorsession::ValidatorSession::GeneratedCandidate> promise) {
  if (round_id > last_known_round_id_) {
    last_known_round_id_ = round_id;
  }
  if (!started_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "cannot collate block: group not started"));
    return;
  }
  if (cached_collated_block_) {
    if (cached_collated_block_->result) {
      promise.set_value({cached_collated_block_->result.value().clone(), true});
    } else {
      cached_collated_block_->promises.push_back(promise.wrap([](BlockCandidate &&res) {
        return validatorsession::ValidatorSession::GeneratedCandidate{std::move(res), true};
      }));
    }
    return;
  }
  cached_collated_block_ = std::make_shared<CachedCollatedBlock>();
  cached_collated_block_->promises.push_back(promise.wrap([](BlockCandidate &&res) {
    return validatorsession::ValidatorSession::GeneratedCandidate{std::move(res), false};
  }));
  td::Promise<BlockCandidate> P = [SelfId = actor_id(this),
                                   cache = cached_collated_block_](td::Result<BlockCandidate> R) {
    td::actor::send_closure(SelfId, &ValidatorGroup::generated_block_candidate, std::move(cache), std::move(R));
  };
  collate_block(round_id, td::Timestamp::in(10.0), std::move(P));
}

void ValidatorGroup::generated_block_candidate(std::shared_ptr<CachedCollatedBlock> cache,
                                               td::Result<BlockCandidate> R) {
  if (R.is_error()) {
    for (auto &p : cache->promises) {
      p.set_error(R.error().clone());
    }
    if (cache == cached_collated_block_) {
      cached_collated_block_ = nullptr;
    }
  } else {
    auto candidate = R.move_as_ok();
    add_available_block_candidate(candidate.pubkey.as_bits256(), candidate.id, candidate.collated_file_hash);
    cache->result = std::move(candidate);
    for (auto &p : cache->promises) {
      p.set_value(cache->result.value().clone());
    }
  }
  cache->promises.clear();
}

void ValidatorGroup::validate_block_candidate(td::uint32 round_id, BlockCandidate block,
                                              td::Promise<std::pair<UnixTime, bool>> promise) {
  if (round_id > last_known_round_id_) {
    last_known_round_id_ = round_id;
  }
  if (round_id < last_known_round_id_) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "too old"));
    return;
  }

  auto next_block_id = create_next_block_id(block.id.root_hash, block.id.file_hash);
  block.id = next_block_id;

  CacheKey cache_key = block_to_cache_key(block);
  auto it = approved_candidates_cache_.find(cache_key);
  if (it != approved_candidates_cache_.end()) {
    promise.set_value({it->second, true});
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), round_id, block = block.clone(),
                                       promise = std::move(promise)](td::Result<ValidateCandidateResult> R) mutable {
    if (R.is_error()) {
      auto S = R.move_as_error();
      if (S.code() != ErrorCode::timeout && S.code() != ErrorCode::notready) {
        LOG(ERROR) << "failed to validate candidate: " << S;
      }
      delay_action(
          [SelfId, round_id, block = std::move(block), promise = std::move(promise)]() mutable {
            td::actor::send_closure(SelfId, &ValidatorGroup::validate_block_candidate, round_id, std::move(block),
                                    std::move(promise));
          },
          td::Timestamp::in(0.1));
    } else {
      auto v = R.move_as_ok();
      v.visit(td::overloaded(
          [&](UnixTime ts) {
            td::actor::send_closure(SelfId, &ValidatorGroup::update_approve_cache, block_to_cache_key(block),
                                    ts);
            td::actor::send_closure(SelfId, &ValidatorGroup::add_available_block_candidate, block.pubkey.as_bits256(),
                                    block.id, block.collated_file_hash);
            promise.set_value({ts, false});
          },
          [&](CandidateReject reject) {
            promise.set_error(
                td::Status::Error(ErrorCode::protoviolation, PSTRING() << "bad candidate: " << reject.reason));
          }));
    }
  });
  if (!started_) {
    P.set_error(td::Status::Error(ErrorCode::notready, "validator group not started"));
    return;
  }
  VLOG(VALIDATOR_DEBUG) << "validating block candidate " << next_block_id;
  run_validate_query(shard_, min_masterchain_block_id_, prev_block_ids_, std::move(block), validator_set_, manager_,
                     td::Timestamp::in(15.0), std::move(P));
}

void ValidatorGroup::update_approve_cache(CacheKey key, UnixTime value) {
  approved_candidates_cache_[key] = value;
}

void ValidatorGroup::accept_block_candidate(td::uint32 round_id, PublicKeyHash src, td::BufferSlice block_data,
                                            RootHash root_hash, FileHash file_hash,
                                            std::vector<BlockSignature> signatures,
                                            std::vector<BlockSignature> approve_signatures,
                                            validatorsession::ValidatorSessionStats stats,
                                            td::Promise<td::Unit> promise) {
  stats.cc_seqno = validator_set_->get_catchain_seqno();
  if (round_id >= last_known_round_id_) {
    last_known_round_id_ = round_id + 1;
  }
  auto sig_set = create_signature_set(std::move(signatures));
  validator_set_->check_signatures(root_hash, file_hash, sig_set).ensure();
  auto approve_sig_set = create_signature_set(std::move(approve_signatures));
  validator_set_->check_approve_signatures(root_hash, file_hash, approve_sig_set).ensure();

  if (!started_) {
    postponed_accept_.push_back(PostponedAccept{root_hash, file_hash, std::move(block_data), std::move(sig_set),
                                                std::move(approve_sig_set), std::move(stats), std::move(promise)});
    return;
  }
  auto next_block_id = create_next_block_id(root_hash, file_hash);
  LOG(WARNING) << "Accepted block " << next_block_id;
  td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, next_block_id, std::move(stats));
  auto block =
      block_data.size() > 0 ? create_block(next_block_id, std::move(block_data)).move_as_ok() : td::Ref<BlockData>{};
  accept_block_query(next_block_id, std::move(block), std::move(prev_block_ids_), std::move(sig_set),
                     std::move(approve_sig_set), src == local_id_, std::move(promise));
  prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
  cached_collated_block_ = nullptr;
  approved_candidates_cache_.clear();
}

void ValidatorGroup::accept_block_query(BlockIdExt block_id, td::Ref<BlockData> block, std::vector<BlockIdExt> prev,
                                        td::Ref<BlockSignatureSet> sig_set, td::Ref<BlockSignatureSet> approve_sig_set,
                                        bool send_broadcast, td::Promise<td::Unit> promise, bool is_retry) {
  auto P = td::PromiseCreator::lambda([=, SelfId = actor_id(this),
                                       promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    if (R.is_error()) {
      if (R.error().code() == ErrorCode::cancelled) {
        promise.set_value(td::Unit());
        return;
      }
      LOG_CHECK(R.error().code() == ErrorCode::timeout || R.error().code() == ErrorCode::notready) << R.move_as_error();
      td::actor::send_closure(SelfId, &ValidatorGroup::accept_block_query, block_id, std::move(block),
                              std::move(prev), std::move(sig_set), std::move(approve_sig_set), send_broadcast,
                              std::move(promise), true);
    } else {
      promise.set_value(R.move_as_ok());
    }
  });

  run_accept_block_query(block_id, std::move(block), std::move(prev), validator_set_, std::move(sig_set),
                         std::move(approve_sig_set), send_broadcast, monitoring_shard_, manager_, std::move(P));
}

void ValidatorGroup::skip_round(td::uint32 round_id) {
  if (round_id >= last_known_round_id_) {
    last_known_round_id_ = round_id + 1;
  }
}

void ValidatorGroup::get_approved_candidate(PublicKey source, RootHash root_hash, FileHash file_hash,
                                            FileHash collated_data_file_hash, td::Promise<BlockCandidate> promise) {
  BlockIdExt id = create_next_block_id(root_hash, file_hash);

  td::actor::send_closure(manager_, &ValidatorManager::get_block_candidate_from_db, source, id, collated_data_file_hash,
                          std::move(promise));
}

BlockIdExt ValidatorGroup::create_next_block_id(RootHash root_hash, FileHash file_hash) const {
  return BlockIdExt{create_next_block_id_simple(), root_hash, file_hash};
}

BlockId ValidatorGroup::create_next_block_id_simple() const {
  BlockSeqno seqno = 0;
  for (auto &p : prev_block_ids_) {
    if (seqno < p.id.seqno) {
      seqno = p.id.seqno;
    }
  }
  return BlockId{shard_.workchain, shard_.shard, seqno + 1};
}

std::unique_ptr<validatorsession::ValidatorSession::Callback> ValidatorGroup::make_validator_session_callback() {
  class Callback : public validatorsession::ValidatorSession::Callback {
   public:
    Callback(td::actor::ActorId<ValidatorGroup> id) : id_(id) {
    }
    void on_candidate(td::uint32 round, PublicKey source, validatorsession::ValidatorSessionRootHash root_hash,
                      td::BufferSlice data, td::BufferSlice collated_data,
                      td::Promise<validatorsession::ValidatorSession::CandidateDecision> promise) override {
      auto P =
          td::PromiseCreator::lambda([promise = std::move(promise)](td::Result<std::pair<td::uint32, bool>> R) mutable {
            if (R.is_ok()) {
              validatorsession::ValidatorSession::CandidateDecision decision(R.ok().first);
              decision.set_is_cached(R.ok().second);
              promise.set_value(std::move(decision));
            } else {
              auto S = R.move_as_error();
              promise.set_value(
                  validatorsession::ValidatorSession::CandidateDecision{S.message().c_str(), td::BufferSlice()});
            }
          });

      BlockCandidate candidate{Ed25519_PublicKey{source.ed25519_value().raw()},
                               BlockIdExt{0, 0, 0, root_hash, sha256_bits256(data.as_slice())},
                               sha256_bits256(collated_data.as_slice()), data.clone(), collated_data.clone()};

      td::actor::send_closure(id_, &ValidatorGroup::validate_block_candidate, round, std::move(candidate),
                              std::move(P));
    }
    void on_generate_slot(td::uint32 round,
                          td::Promise<validatorsession::ValidatorSession::GeneratedCandidate> promise) override {
      td::actor::send_closure(id_, &ValidatorGroup::generate_block_candidate, round, std::move(promise));
    }
    void on_block_committed(td::uint32 round, PublicKey source, validatorsession::ValidatorSessionRootHash root_hash,
                            validatorsession::ValidatorSessionFileHash file_hash, td::BufferSlice data,
                            std::vector<std::pair<PublicKeyHash, td::BufferSlice>> signatures,
                            std::vector<std::pair<PublicKeyHash, td::BufferSlice>> approve_signatures,
                            validatorsession::ValidatorSessionStats stats) override {
      std::vector<BlockSignature> sigs;
      for (auto &sig : signatures) {
        sigs.emplace_back(BlockSignature{sig.first.bits256_value(), std::move(sig.second)});
      }
      std::vector<BlockSignature> approve_sigs;
      for (auto &sig : approve_signatures) {
        approve_sigs.emplace_back(BlockSignature{sig.first.bits256_value(), std::move(sig.second)});
      }
      auto P = td::PromiseCreator::lambda([](td::Result<td::Unit>) {});
      td::actor::send_closure(id_, &ValidatorGroup::accept_block_candidate, round, source.compute_short_id(),
                              std::move(data), root_hash, file_hash, std::move(sigs), std::move(approve_sigs),
                              std::move(stats), std::move(P));
    }
    void on_block_skipped(td::uint32 round) override {
      td::actor::send_closure(id_, &ValidatorGroup::skip_round, round);
    }
    void get_approved_candidate(PublicKey source, validatorsession::ValidatorSessionRootHash root_hash,
                                validatorsession::ValidatorSessionFileHash file_hash,
                                validatorsession::ValidatorSessionCollatedDataFileHash collated_data_file_hash,
                                td::Promise<BlockCandidate> promise) override {
      td::actor::send_closure(id_, &ValidatorGroup::get_approved_candidate, source, root_hash, file_hash,
                              collated_data_file_hash, std::move(promise));
    }

   private:
    td::actor::ActorId<ValidatorGroup> id_;
  };

  return std::make_unique<Callback>(actor_id(this));
}

void ValidatorGroup::create_session() {
  CHECK(!init_);
  init_ = true;
  std::vector<validatorsession::ValidatorSessionNode> vec;
  auto v = validator_set_->export_vector();
  bool found = false;
  for (auto &el : v) {
    validatorsession::ValidatorSessionNode n;
    n.pub_key = ValidatorFullId{el.key};
    n.weight = el.weight;
    if (el.addr.is_zero()) {
      n.adnl_id = adnl::AdnlNodeIdShort{n.pub_key.compute_short_id()};
    } else {
      n.adnl_id = adnl::AdnlNodeIdShort{el.addr};
    }
    if (n.pub_key.compute_short_id() == local_id_) {
      CHECK(!found);
      found = true;
      local_id_full_ = n.pub_key;
      local_adnl_id_ = n.adnl_id;
    }
    vec.emplace_back(std::move(n));
  }
  CHECK(found);

  if (!config_.new_catchain_ids) {
    session_ = validatorsession::ValidatorSession::create(session_id_, config_, local_id_, std::move(vec),
                                                          make_validator_session_callback(), keyring_, adnl_, rldp_,
                                                          overlays_, db_root_, "-", allow_unsafe_self_blocks_resync_);
  } else {
    session_ = validatorsession::ValidatorSession::create(
        session_id_, config_, local_id_, std::move(vec), make_validator_session_callback(), keyring_, adnl_, rldp_,
        overlays_, db_root_ + "/catchains/",
        PSTRING() << "." << shard_.workchain << "." << shard_.shard << "." << validator_set_->get_catchain_seqno()
                  << ".",
        allow_unsafe_self_blocks_resync_);
  }
  double catchain_delay = opts_->get_catchain_max_block_delay() ? opts_->get_catchain_max_block_delay().value() : 0.4;
  double catchain_delay_slow =
      std::max(catchain_delay,
               opts_->get_catchain_max_block_delay_slow() ? opts_->get_catchain_max_block_delay_slow().value() : 1.0);
  td::actor::send_closure(session_, &validatorsession::ValidatorSession::set_catchain_max_block_delay, catchain_delay,
                          catchain_delay_slow);
  if (started_) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::start);
  }

  td::actor::send_closure(rldp_, &rldp::Rldp::add_id, local_adnl_id_);
}

void ValidatorGroup::start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id) {
  prev_block_ids_ = prev;
  min_masterchain_block_id_ = min_masterchain_block_id;
  cached_collated_block_ = nullptr;
  approved_candidates_cache_.clear();
  started_ = true;

  if (init_) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::start);
  }

  for (auto &p : postponed_accept_) {
    auto next_block_id = create_next_block_id(p.root_hash, p.file_hash);
    td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, next_block_id,
                            std::move(p.stats));

    auto block =
        p.block.size() > 0 ? create_block(next_block_id, std::move(p.block)).move_as_ok() : td::Ref<BlockData>{};
    accept_block_query(next_block_id, std::move(block), std::move(prev_block_ids_), std::move(p.sigs),
                       std::move(p.approve_sigs), false, std::move(p.promise));
    prev_block_ids_ = std::vector<BlockIdExt>{next_block_id};
  }
  postponed_accept_.clear();

  validatorsession::NewValidatorGroupStats stats;
  stats.session_id = session_id_;
  stats.shard = shard_;
  stats.cc_seqno = validator_set_->get_catchain_seqno();
  stats.last_key_block_seqno = last_key_block_seqno_;
  stats.timestamp = td::Clocks::system();
  td::uint32 idx = 0;
  for (const auto& node : validator_set_->export_vector()) {
    PublicKeyHash id = ValidatorFullId{node.key}.compute_short_id();
    if (id == local_id_) {
      stats.self_idx = idx;
    }
    stats.nodes.push_back(validatorsession::NewValidatorGroupStats::Node{id, node.weight});
    ++idx;
  }
  td::actor::send_closure(manager_, &ValidatorManager::log_new_validator_group_stats, std::move(stats));
}

void ValidatorGroup::destroy() {
  if (!session_.empty()) {
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::get_current_stats,
                            [manager = manager_, cc_seqno = validator_set_->get_catchain_seqno(),
                             block_id = create_next_block_id(RootHash::zero(), FileHash::zero())](
                                td::Result<validatorsession::ValidatorSessionStats> R) {
                              if (R.is_error()) {
                                LOG(WARNING) << "Failed to get validator session stats: " << R.move_as_error();
                                return;
                              }
                              auto stats = R.move_as_ok();
                              if (stats.rounds.empty()) {
                                return;
                              }
                              stats.cc_seqno = cc_seqno;
                              td::actor::send_closure(manager, &ValidatorManager::log_validator_session_stats, block_id,
                                                      std::move(stats));
                            });
    td::actor::send_closure(session_, &validatorsession::ValidatorSession::get_end_stats,
                            [manager = manager_](td::Result<validatorsession::EndValidatorGroupStats> R) {
                              if (R.is_error()) {
                                LOG(DEBUG) << "Failed to get validator session end stats: " << R.move_as_error();
                                return;
                              }
                              auto stats = R.move_as_ok();
                              td::actor::send_closure(manager, &ValidatorManager::log_end_validator_group_stats,
                                                      std::move(stats));
                            });
    auto ses = session_.release();
    delay_action([ses]() mutable { td::actor::send_closure(ses, &validatorsession::ValidatorSession::destroy); },
                 td::Timestamp::in(10.0));
  }
  stop();
}

void ValidatorGroup::get_validator_group_info_for_litequery(
    td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise) {
  if (session_.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::notready, "not started"));
    return;
  }
  td::actor::send_closure(
      session_, &validatorsession::ValidatorSession::get_validator_group_info_for_litequery, last_known_round_id_,
      [SelfId = actor_id(this), promise = std::move(promise), round = last_known_round_id_](
          td::Result<std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>>> R) mutable {
        TRY_RESULT_PROMISE(promise, result, std::move(R));
        td::actor::send_closure(SelfId, &ValidatorGroup::get_validator_group_info_for_litequery_cont, round,
                                std::move(result), std::move(promise));
      });
}

void ValidatorGroup::get_validator_group_info_for_litequery_cont(
    td::uint32 expected_round, std::vector<tl_object_ptr<lite_api::liteServer_nonfinal_candidateInfo>> candidates,
    td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise) {
  if (expected_round != last_known_round_id_) {
    candidates.clear();
  }

  BlockId next_block_id = create_next_block_id_simple();
  for (auto &candidate : candidates) {
    BlockIdExt id{next_block_id, candidate->id_->block_id_->root_hash_, candidate->id_->block_id_->file_hash_};
    candidate->id_->block_id_ = create_tl_lite_block_id(id);
    candidate->available_ =
        available_block_candidates_.count({candidate->id_->creator_, id, candidate->id_->collated_data_hash_});
  }

  auto result = create_tl_object<lite_api::liteServer_nonfinal_validatorGroupInfo>();
  result->next_block_id_ = create_tl_lite_block_id_simple(next_block_id);
  for (const BlockIdExt& prev : prev_block_ids_) {
    result->prev_.push_back(create_tl_lite_block_id(prev));
  }
  result->cc_seqno_ = validator_set_->get_catchain_seqno();
  result->candidates_ = std::move(candidates);
  promise.set_result(std::move(result));
}

void ValidatorGroup::collate_block(td::uint32 round_id, td::Timestamp timeout, td::Promise<BlockCandidate> promise,
                                   unsigned max_retries) {
  if (round_id < last_known_round_id_) {
    promise.set_error(td::Status::Error("too old"));
    return;
  }
  BlockId next_block_id = create_next_block_id_simple();
  adnl::AdnlNodeIdShort collator_adnl_id = adnl::AdnlNodeIdShort::zero();
  bool self_collate = false;
  bool trusted_collator = false;

  if (shard_.is_masterchain()) {
    self_collate = true;
  } else {
    for (const auto &s : opts_->get_collators_list()->shards) {
      if (!shard_intersects(s.shard_id, shard_)) {
        continue;
      }
      if (!s.collators.empty()) {
        const CollatorsList::Collator &col = s.collators[td::Random::fast(0, s.collators.size() - 1)];
        collator_adnl_id = col.adnl_id;
        trusted_collator = col.trusted;
        break;
      }
    }
    if (collator_adnl_id.is_zero()) {
      if (opts_->get_collators_list()->self_collate) {
        self_collate = true;
      } else if (opts_->get_collators_list()->use_config_41) {
        // TODO: some way to choose node (similar to "unreliability" in full-node)
        int cnt = 0;
        for (const block::CollatorNodeDescr &c : collator_config_.collator_nodes) {
          if (shard_intersects(shard_, c.shard)) {
            if (td::Random::fast(0, cnt) == 0) {
              collator_adnl_id = adnl::AdnlNodeIdShort(c.adnl_id);
            }
            ++cnt;
          }
        }
      }
    }
  }
  if (self_collate) {
    run_collate_query(shard_, min_masterchain_block_id_, prev_block_ids_,
                      Ed25519_PublicKey{local_id_full_.ed25519_value().raw()}, validator_set_,
                      opts_->get_collator_options(), manager_, td::Timestamp::in(10.0), std::move(promise));
    return;
  }
  if (collator_adnl_id.is_zero()) {
    promise.set_error(td::Status::Error(PSTRING() << "no collator for shard " << shard_.to_str()));
    return;
  }

  promise = td::PromiseCreator::lambda([=, SelfId = actor_id(this), promise = std::move(promise),
                                        timer = td::Timer()](td::Result<BlockCandidate> R) mutable {
    if (R.is_ok()) {
      LOG(INFO) << "collate query for " << next_block_id.to_str() << ": success, time=" << timer.elapsed() << "s";
      promise.set_result(R.move_as_ok());
      return;
    }
    bool retry = (!timeout || !timeout.is_in_past()) && max_retries > 0;
    LOG(WARNING) << "collate query for " << next_block_id.to_str() << ": " << R.error() << ", time=" << timer.elapsed()
                 << "s, " << (retry ? "retrying" : "giving up");
    if (retry) {
      td::actor::send_closure(SelfId, &ValidatorGroup::collate_block, round_id, timeout, std::move(promise),
                              max_retries - 1);
    } else {
      promise.set_result(td::Status::Error(ErrorCode::timeout, "timeout"));
    }
  });

  std::vector<tl_object_ptr<ton_api::tonNode_blockIdExt>> prev_blocks;
  for (const BlockIdExt &p : prev_block_ids_) {
    prev_blocks.push_back(create_tl_block_id(p));
  }
  td::BufferSlice query = create_serialize_tl_object<ton_api::collatorNode_generateBlock>(
      create_tl_shard_id(shard_), validator_set_->get_catchain_seqno(), std::move(prev_blocks),
      local_id_full_.ed25519_value().raw());

  auto P = td::PromiseCreator::lambda(
      [=, SelfId = actor_id(this), promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix("rldp query failed: "));
          return;
        }
        td::actor::send_closure(SelfId, &ValidatorGroup::receive_collate_query_response, round_id, R.move_as_ok(),
                                trusted_collator, std::move(promise));
      });
  LOG(INFO) << "sending collate query for " << next_block_id.to_str() << ": send to " << collator_adnl_id;
  size_t max_answer_size = config_.max_block_size + config_.max_collated_data_size + 1024;
  td::Timestamp query_timeout = td::Timestamp::in(10.0);
  query_timeout.relax(timeout);
  td::actor::send_closure(rldp_, &rldp::Rldp::send_query_ex, local_adnl_id_, collator_adnl_id, "collatequery",
                          std::move(P), timeout, std::move(query), max_answer_size);
}

void ValidatorGroup::receive_collate_query_response(td::uint32 round_id, td::BufferSlice data, bool trusted_collator,
                                                    td::Promise<BlockCandidate> promise) {
  if (round_id < last_known_round_id_) {
    promise.set_error(td::Status::Error("too old"));
    return;
  }
  TRY_RESULT_PROMISE(promise, f, fetch_tl_object<ton_api::collatorNode_GenerateBlockResult>(data, true));
  td::Result<BlockCandidate> res;
  ton_api::downcast_call(*f, td::overloaded(
      [&](ton_api::collatorNode_generateBlockError &r) {
        td::Status error = td::Status::Error(r.code_, r.message_);
        res = error.move_as_error_prefix("collate query: ");
      },
      [&](ton_api::collatorNode_generateBlockSuccess &r) {
        res = CollatorNode::deserialize_candidate(
            std::move(r.candidate_),
            config_.max_block_size + config_.max_collated_data_size + 1024);
      }));
  TRY_RESULT_PROMISE(promise, candidate, std::move(res));
  if (candidate.pubkey.as_bits256() != local_id_full_.ed25519_value().raw()) {
    promise.set_error(td::Status::Error("collate query: block candidate source mismatch"));
    return;
  }
  if (candidate.id.shard_full() != shard_) {
    promise.set_error(td::Status::Error("collate query: shard mismatch"));
    return;
  }

  if (trusted_collator) {
    promise.set_result(std::move(candidate));
    return;
  }
  auto P = td::PromiseCreator::lambda(
      [candidate = candidate.clone(), promise = std::move(promise)](td::Result<std::pair<UnixTime, bool>> R) mutable {
        if (R.is_error()) {
          promise.set_error(R.move_as_error_prefix("validate received block error: "));
          return;
        }
        promise.set_result(std::move(candidate));
      });
  validate_block_candidate(round_id, std::move(candidate), std::move(P));
}

}  // namespace validator

}  // namespace ton
