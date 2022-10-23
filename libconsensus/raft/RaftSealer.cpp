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
#include <memory>
#include <thread>

using namespace std;
using namespace dev;
using namespace dev::consensus;

/// fake single transaction
Transaction::Ptr fakeTransaction(size_t _idx = 0)
{
    u256 value = u256(42);
    u256 gas = u256(100000000);
    u256 gasPrice = u256(0);
    Address dst;
    std::string str =
        "test transaction for CommonTransactionNonceCheck" + std::to_string(utcTime());
    bytes data(str.begin(), str.end());
    u256 const& nonce = u256(utcTime() + _idx);
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
    uint64_t poolLimit = 20000LL;
    uint64_t memLimit = 1024LL * 1024 * 1024 * 4;
    // 设置交易池最大容量为20000
    txPool->setTxPoolLimit(poolLimit);
    txPool->setMaxMemoryLimit(memLimit);

    auto txBoost = [=](int id) {
        std::this_thread::sleep_for(std::chrono::seconds{3});
        int i = 0;
        while (true)
        {
            if (txPool->isFull())
            {
                LOG(INFO) << LOG_DESC("[zd]") << LOG_KV("txPool.size", txPool->pendingSize());
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                continue;
            }
            auto tx = fakeTransaction(i);
            auto res = txPool->submitTransactions(tx);
            LOG(INFO) << LOG_DESC("[zd]") << LOG_KV("id", id) << LOG_KV("index", i)
                      << LOG_KV("txhash", res.first) << LOG_KV("cap", tx->capacity())
                      << LOG_KV("pendingSize", txPool->pendingSize())
                      << LOG_KV("txPool", txPool->maxBlockLimit());
        }
    };
    std::thread{txBoost, 0}.detach();

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
                         << LOG_KV("txNum", m_sealing.block->getTransactionSize())
                         << LOG_KV("hash", m_sealing.block->header().hash().abridged());

    if (m_sealing.block->getTransactionSize() == 0)
    {
        RAFTSEALER_LOG(TRACE) << LOG_DESC("[handleBlock]Empty block will not be committed");
        reset();
        m_raftEngine->resetLastBlockTime();
        return;
    }
    auto before = std::chrono::steady_clock::now();
    bool succ = m_raftEngine->commit(*(m_sealing.block));
    double take = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now() - before)
                      .count();
    LOG(INFO) << LOG_DESC("[zd]") << LOG_KV("height", m_sealing.block->header().number())
              << LOG_KV("consensus take", take) << endl;
    if (!succ)
    {
        reset();
    }
}
