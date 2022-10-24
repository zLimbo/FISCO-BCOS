/*
 * @CopyRight:
 * FISCO-BCOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FISCO-BCOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>
 * (c) 2016-2018 fisco-dev contributors.
 */

/**
 * @brief : implementation of Raft consensus sealer
 * @file: RaftSealer.cpp
 * @author: catli
 * @date: 2018-12-05
 */
#include "RaftSealer.h"
#include "libtxpool/TxPool.h"
#include <chrono>
#include <cstdint>
#include <thread>

using namespace std;
using namespace dev;
using namespace dev::consensus;

static atomic<uint64_t> aTxCnt{0};

/// fake single transaction
Transaction::Ptr fakeTransaction()
{
    u256 value = u256(42);
    u256 gas = u256(100000000);
    u256 gasPrice = u256(0);
    Address dst;
    std::string str = string(512, 'x') + std::to_string(utcTime());
    bytes data(str.begin(), str.end());
    u256 const& nonce = u256(aTxCnt++);
    Transaction::Ptr fakeTx = std::make_shared<Transaction>(value, gasPrice, gas, dst, data, nonce);

    auto keyPair = KeyPair::create();
    std::shared_ptr<crypto::Signature> sig =
        dev::crypto::Sign(keyPair, fakeTx->hash(WithoutSignature));
    /// update the signature of transaction
    fakeTx->updateSignature(sig);
    return fakeTx;
}


void RaftSealer::start()
{
    auto txPool = static_pointer_cast<txpool::TxPool>(m_txPool);
    auto txBoost = [this, txPool](int id) {
        std::this_thread::sleep_for(std::chrono::seconds{3});
        while (true)
        {
            if (!m_raftEngine->isLeader())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{1000});
                continue;
            }
            if (txPool->isFull())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                continue;
            }
            auto tx = fakeTransaction();
            auto res = txPool->submit(tx);
            if (aTxCnt % 5000)
                continue;
            LOG(INFO) << LOG_DESC("zd") << LOG_KV("id", id) << LOG_KV("aTxCnt", aTxCnt)
                      << LOG_KV("hash", res.first) << LOG_KV("addr", res.second)
                      << LOG_KV("cap", tx->capacity())
                      << LOG_KV("pendingSize", txPool->pendingSize())
                      << LOG_KV("maxBlockLimit", txPool->maxBlockLimit());
        }
    };
    std::thread{txBoost, 0}.detach();
    // std::thread{txBoost, 1}.detach();
    // std::thread{txBoost, 2}.detach();

    m_raftEngine->start();
    Sealer::start();
}

void RaftSealer::stop()
{
    Sealer::stop();
    m_raftEngine->stop();
}

bool RaftSealer::shouldSeal()
{
    return Sealer::shouldSeal() && m_raftEngine->shouldSeal();
}

bool RaftSealer::reachBlockIntervalTime()
{
    return m_raftEngine->reachBlockIntervalTime();
}

void RaftSealer::handleBlock()
{
    resetSealingHeader(m_sealing.block->header());
    m_sealing.block->calTransactionRoot();

    RAFTSEALER_LOG(INFO) << LOG_DESC("[handleBlock]++++++++++++++++ Generating seal")
                         << LOG_KV("blockNumber", m_sealing.block->header().number())
                         << LOG_KV("hash", m_sealing.block->header().hash().abridged())
                         << LOG_KV("now", utcTime())
                         << LOG_KV("height", m_sealing.block->blockHeader().number())
                         << LOG_KV("txNum", m_sealing.block->transactions()->size())
                         << LOG_KV("pendingSize", m_txPool->pendingSize());


    if (m_sealing.block->getTransactionSize() == 0)
    {
        RAFTSEALER_LOG(TRACE) << LOG_DESC("[handleBlock]Empty block will not be committed");
        reset();
        m_raftEngine->resetLastBlockTime();
        return;
    }

    using namespace std::chrono;
    using Seconds = duration<double>;

    auto block = m_sealing.block;
    auto before = steady_clock::now();
    auto txNum = block->transactions()->size();
    LOG(INFO) << LOG_BADGE("zd") << LOG_DESC("before commit")
              << LOG_KV("height", block->blockHeader().number()) << LOG_KV("txNum", txNum);

    bool succ = m_raftEngine->commit(*(m_sealing.block));

    double consensusTake = duration_cast<Seconds>(steady_clock::now() - before).count();


    static bool isFirst = false;
    static steady_clock::time_point last;
    static double totalTxNum = 0.0;
    if (!isFirst)
    {
        isFirst = true;
        last = steady_clock::now();
    }
    else
    {
        static steady_clock::time_point start = steady_clock::now();
        auto cur = steady_clock::now();
        double take = duration_cast<Seconds>(cur - last).count();
        double tps = static_cast<double>(txNum) / take;
        last = cur;
        totalTxNum += static_cast<double>(txNum);
        double totalTake = duration_cast<Seconds>(cur - start).count();
        double totalTps = totalTxNum / totalTake;
        LOG(INFO) << LOG_BADGE("zd") << LOG_DESC("statistics")
                  << LOG_KV("height", block->blockHeader().number()) << LOG_KV("txNum", txNum)
                  << LOG_KV("totalTxNum", totalTxNum) << LOG_KV("aTxCnt", aTxCnt)
                  << LOG_KV("take(s)", take) << LOG_KV("tps", tps) << LOG_KV("totalTps", totalTps)
                  << LOG_KV("consensus take", consensusTake);
    }


    if (!succ)
    {
        reset();
    }
}
