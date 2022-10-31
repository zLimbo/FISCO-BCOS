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
 * @brief : implementation of Consensus
 * @file: Consensus.cpp
 * @author: yujiechen
 * @date: 2018-09-27
 *
 * @ author: yujiechen
 * @ date: 2018-10-26
 * @ file : Sealer.cpp
 * @ modification: rename Consensus.cpp to Sealer.cpp
 */
#include "Sealer.h"
#include <libethcore/LogEntry.h>
#include <libsync/SyncStatus.h>
using namespace std;
using namespace dev::sync;
using namespace dev::blockverifier;
using namespace dev::eth;
using namespace dev::p2p;
using namespace dev::consensus;

/// start the Sealer module
void Sealer::start()
{
    if (m_startConsensus)
    {
        SEAL_LOG(WARNING) << "[Sealer module has already been started]";
        return;
    }
    SEAL_LOG(INFO) << "[Start sealer module]";
    resetSealingBlock();
    m_consensusEngine->reportBlock(*(m_blockChain->getBlockByNumber(m_blockChain->number())));
    m_maxBlockCanSeal = m_consensusEngine->maxBlockTransactions();
    m_syncBlock = false;
    /// start  a thread to execute doWork()&&workLoop()
    startWorking();
    m_startConsensus = true;
}

bool Sealer::shouldSeal()
{
    bool sealed = false;
    {
        ReadGuard l(x_sealing);
        sealed = m_sealing.block->isSealed();
    }
    return (!sealed && m_startConsensus &&
            m_consensusEngine->accountType() == NodeAccountType::SealerAccount &&
            !isBlockSyncing());
}

void Sealer::reportNewBlock()
{
    bool t = true;
    if (m_syncBlock.compare_exchange_strong(t, false))
    {
        shared_ptr<dev::eth::Block> p_block =
            m_blockChain->getBlockByNumber(m_blockChain->number());
        if (!p_block)
        {
            LOG(ERROR) << "[reportNewBlock] empty block";
            return;
        }
        m_consensusEngine->reportBlock(*p_block);
        WriteGuard l(x_sealing);
        {
            if (shouldResetSealing())
            {
                SEAL_LOG(DEBUG) << "[reportNewBlock] Reset sealing: [number]:  "
                                << m_blockChain->number()
                                << ", sealing number:" << m_sealing.block->blockHeader().number();
                resetSealingBlock();
            }
        }
    }
}

bool Sealer::shouldWait(bool const& wait) const
{
    return !m_syncBlock && wait;
}

struct ZFakeTxs
{
    ZFakeTxs(int n) : threadNum_(n)
    {
        while (n--)
        {
            ths_.emplace_back([this] { genTxs(); });
        }
    }

    /// fake single transaction
    Transaction::Ptr fakeTransaction()
    {
        using namespace dev;
        u256 value = u256(42);
        u256 gas = u256(100000000);
        u256 gasPrice = u256(0);
        Address dst;
        std::string str = string(512, 'x') + std::to_string(utcTime());
        bytes data(str.begin(), str.end());
        u256 const& nonce = u256(aTxCnt_++);
        Transaction::Ptr fakeTx =
            std::make_shared<Transaction>(value, gasPrice, gas, dst, data, nonce);

        auto keyPair = KeyPair::create();
        std::shared_ptr<crypto::Signature> sig =
            dev::crypto::Sign(keyPair, fakeTx->hash(WithoutSignature));
        /// update the signature of transaction
        fakeTx->updateSignature(sig);
        return fakeTx;
    }

    std::shared_ptr<Transactions> getTxs()
    {
        std::unique_lock<std::mutex> lock{mu_};
        cond_.wait(lock, [this] { return !txsQueue_.empty(); });
        auto ret = txsQueue_.front();
        txsQueue_.pop();
        freeCnt_ -= kMaxSealNum;
        return ret;
    }

    void genTxs()
    {
        while (true)
        {
            if (txsQueueSize() >= threadNum_ * 2)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                continue;
            }
            auto txs = std::make_shared<Transactions>();
            txs->reserve(kMaxSealNum);
            for (int i = 0; i < kMaxSealNum; ++i)
            {
                txs->push_back(fakeTransaction());
            }
            std::unique_lock<std::mutex> lock{mu_};
            txsQueue_.push(txs);
            freeCnt_ += kMaxSealNum;
            cond_.notify_one();
        }
    }

    int txsQueueSize()
    {
        std::unique_lock<std::mutex> lock{mu_};
        return txsQueue_.size();
    }

    const int kMaxSealNum = 5000;
    std::mutex mu_;
    std::condition_variable cond_;
    std::queue<std::shared_ptr<Transactions>> txsQueue_;
    atomic<uint64_t> aTxCnt_{0};
    atomic<uint64_t> freeCnt_{0};
    std::vector<std::thread> ths_;
    int threadNum_;
};

#include <libconsensus/raft/Common.h>
#include <libconsensus/raft/RaftSealer.h>
#include <libconsensus/raft/RaftEngine.h>

void Sealer::doWork(bool wait)
{
    reportNewBlock();
    if (shouldSeal() && m_startConsensus.load())
    {
        WriteGuard l(x_sealing);
        {
            /// get current transaction num
            // uint64_t tx_num = m_sealing.block->getTransactionSize();

            // /// add this to in case of unlimited-loop
            // if (m_txPool->status().current == 0)
            // {
            //     m_syncTxPool = false;
            // }
            // else
            // {
            //     m_syncTxPool = true;
            // }
            // auto maxTxsPerBlock = maxBlockCanSeal();
            // /// load transaction from transaction queue
            // if (maxTxsPerBlock > tx_num && m_syncTxPool == true && !reachBlockIntervalTime())
            //     loadTransactions(maxTxsPerBlock - tx_num);
            // /// check enough or reach block interval
            // if (!checkTxsEnough(maxTxsPerBlock))
            // {
            //     ///< 10 milliseconds to next loop
            //     boost::unique_lock<boost::mutex> l(x_signalled);
            //     m_signalled.wait_for(l, boost::chrono::milliseconds(1));
            //     return;
            // }


            /////////////////////////////////////////////////
            //
            static ZFakeTxs zFakeTxs{2};  // 线程池生产交易队列

            using namespace std::chrono;
            using Seconds = duration<double>;
            auto before = steady_clock::now();
            auto txs = zFakeTxs.getTxs();
            m_sealing.block->appendTransactions(txs);
            double take = duration_cast<Seconds>(steady_clock::now() - before).count();

            auto rfSealer = static_pointer_cast<RaftSealer>(shared_from_this());
            auto rfEng = rfSealer->getEngine();

            auto dtos = [](double d) {
                    char buf[64];
                    sprintf(buf, "%.2f", d);
                    return std::string(buf);
                };

            LOG(INFO) << LOG_DESC("zd seal block")
                      << LOG_KV("| aTxCnt", zFakeTxs.aTxCnt_) 
                      << LOG_KV("| freeCnt", zFakeTxs.freeCnt_)
                      << LOG_KV("| term", rfEng->getTerm())
                      << LOG_KV("| me", rfEng->getNodeIdx())
                      << LOG_KV("| leader", rfEng->getLeader())
                      << LOG_KV("| height", m_sealing.block->header().number())
                      << LOG_KV("| txNum", m_sealing.block->getTransactionSize())
                      << LOG_KV("| take", dtos(take));

            //
            /////////////////////////////////

            if (shouldHandleBlock())
                handleBlock();
        }
    }
    if (shouldWait(wait))
    {
        boost::unique_lock<boost::mutex> l(x_blocksignalled);
        m_blockSignalled.wait_for(l, boost::chrono::milliseconds(10));
    }
}

/**
 * @brief: load transactions from the transaction pool
 * @param transToFetch: max transactions to fetch
 */
void Sealer::loadTransactions(uint64_t const& transToFetch)
{
    /// fetch transactions and update m_transactionSet
    m_sealing.block->appendTransactions(
        m_txPool->topTransactions(transToFetch, m_sealing.m_transactionSet, true));
}

/// check whether the blocksync module is syncing
bool Sealer::isBlockSyncing()
{
    SyncStatus state = m_blockSync->status();
    return (state.state != SyncState::Idle);
}

/**
 * @brief : reset specified sealing block by generating an empty block
 *
 * @param sealing :  the block should be resetted
 * @param filter : the tx hashes of transactions that should't be packeted into sealing block when
 * loadTransactions(used to set m_transactionSet)
 * @param resetNextLeader : reset realing for the next leader or not ? default is false.
 *                          true: reset sealing for the next leader; the block number of the sealing
 * header should be reset to the current block number add 2 false: reset sealing for the current
 * leader; the sealing header should be populated from the current block
 */
void Sealer::resetSealingBlock(Sealing& sealing, h256Hash const& filter, bool resetNextLeader)
{
    resetBlock(sealing.block, resetNextLeader);
    sealing.m_transactionSet = filter;
    sealing.p_execContext = nullptr;
}

/**
 * @brief : reset specified block according to 'resetNextLeader' option
 *
 * @param block : the block that should be resetted
 * @param resetNextLeader: reset the block for the next leader or not ? default is false.
 *                         true: reset block for the next leader; the block number of the block
 * header should be reset to the current block number add 2 false: reset block for the current
 * leader; the block header should be populated from the current block
 */
void Sealer::resetBlock(std::shared_ptr<dev::eth::Block> block, bool resetNextLeader)
{
    /// reset block for the next leader:
    /// 1. clear the block; 2. set the block number to current block number add 2
    if (resetNextLeader)
    {
        SEAL_LOG(DEBUG) << "reset nextleader number to:" << (m_blockChain->number() + 2);
        block->resetCurrentBlock();
        block->header().setNumber(m_blockChain->number() + 2);
    }
    /// reset block for current leader:
    /// 1. clear the block; 2. populate header from the highest block
    else
    {
        auto highestBlock = m_blockChain->getBlockByNumber(m_blockChain->number());
        if (!highestBlock)
        {  // impossible so exit
            SEAL_LOG(FATAL) << LOG_DESC("exit because can't get highest block")
                            << LOG_KV("number", m_blockChain->number());
        }
        block->resetCurrentBlock(highestBlock->blockHeader());
        SEAL_LOG(DEBUG) << "resetCurrentBlock to"
                        << LOG_KV("sealingNum", block->blockHeader().number());
    }
}

/**
 * @brief : set some important fields for specified block header (called by PBFTSealer after load
 * transactions finished)
 *
 * @param header : the block header should be setted
 * the resetted fields including to:
 * 1. block import time;
 * 2. sealer list: reset to current leader list
 * 3. sealer: reset to the idx of the block generator
 */
void Sealer::resetSealingHeader(BlockHeader& header)
{
    /// import block
    resetCurrentTime();
    header.setSealerList(m_consensusEngine->consensusList());
    header.setSealer(m_consensusEngine->nodeIdx());
    header.setLogBloom(LogBloom());
    header.setGasUsed(u256(0));
    header.setExtraData(m_extraData);
}

/// stop the Sealer module
void Sealer::stop()
{
    if (m_startConsensus == false)
    {
        return;
    }
    SEAL_LOG(INFO) << "Stop sealer module...";
    m_startConsensus = false;
    doneWorking();
    if (isWorking())
    {
        stopWorking();
        // will not restart worker, so terminate it
        terminate();
    }
}
