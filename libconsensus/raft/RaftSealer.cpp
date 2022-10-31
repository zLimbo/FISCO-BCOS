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

using namespace std;
using namespace dev;
using namespace dev::consensus;


void RaftSealer::start()
{
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

    using namespace std::chrono;
    using Seconds = duration<double>;

    auto block = m_sealing.block;
    auto before = steady_clock::now();
    auto txNum = block->transactions()->size();

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
        auto rfEng = m_raftEngine;

        auto dtos = [](double d) {
                    char buf[64];
                    sprintf(buf, "%.2f", d);
                    return std::string(buf);
                };
        LOG(INFO) << LOG_DESC("zd stat")
                    << LOG_KV("| term", rfEng->getTerm())
                      << LOG_KV("| me", rfEng->getNodeIdx())
                      << LOG_KV("| leader", rfEng->getLeader())
                  << LOG_KV("| height", block->blockHeader().number()) 
                  << LOG_KV("| txNum", txNum)
                  << LOG_KV("take(s)", dtos(take))
                  << LOG_KV("| tps", dtos(tps)) << LOG_KV("totalTps", dtos(totalTps))
                  << LOG_KV("| consensus take(s)", dtos(consensusTake));
    }
    
    if (!succ)
    {
        reset();
    }
}
