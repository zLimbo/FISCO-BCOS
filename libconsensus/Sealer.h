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
 * @file: Consensus.h
 * @author: yujiechen
 * @date: 2018-09-27
 *
 * @ author: yujiechen
 * @ date: 2018-10-26
 * @ file: Sealer.h
 * @ modification: rename Consensus.h to Sealer.h
 */
#pragma once
#include "ConsensusEngineBase.h"
#include <libblockchain/BlockChainInterface.h>
#include <libdevcore/Worker.h>
#include <libethcore/Block.h>
#include <libethcore/Exceptions.h>
#include <libsync/SyncInterface.h>
#include <libtxpool/TxPool.h>
#include <libtxpool/TxPoolInterface.h>
namespace dev
{
namespace consensus
{
class Sealer : public Worker
{
public:
    using Ptr = std::shared_ptr<Sealer>;
    /**
     * @param _service: p2p service module
     * @param _txPool: transaction pool module
     * @param _blockChain: block chain module
     * @param _blockSync: block sync module
     * @param _blockVerifier: block verifier module
     * @param _protocolId: protocolId
     * @param _sealerList: sealers to generate and execute block
     */
    Sealer(std::shared_ptr<dev::txpool::TxPoolInterface> _txPool,
        std::shared_ptr<dev::blockchain::BlockChainInterface> _blockChain,
        std::shared_ptr<dev::sync::SyncInterface> _blockSync)
      : Worker("Sealer", 0),
        m_txPool(_txPool),
        m_blockSync(_blockSync),
        m_blockChain(_blockChain),
        m_consensusEngine(nullptr)
    {
        assert(m_txPool && m_blockSync && m_blockChain);
        if (m_txPool->status().current > 0)
        {
            m_syncTxPool = true;
        }

        /// register a handler to be called once new transactions imported
        m_tqReady = m_txPool->onReady([=]() { this->onTransactionQueueReady(); });
        m_blockSubmitted = m_blockChain->onReady([=](int64_t) { this->onBlockChanged(); });
    }

    virtual ~Sealer() noexcept { stop(); }
    /// start the Sealer module
    virtual void start();
    /// stop the Sealer module
    virtual void stop();
    /// Magically called when m_tq needs syncing. Be nice and don't block.
    virtual void onTransactionQueueReady()
    {
        m_syncTxPool = true;
        m_signalled.notify_all();
        m_blockSignalled.notify_all();
    }
    virtual void onBlockChanged()
    {
        m_syncBlock = true;
        m_signalled.notify_all();
        m_blockSignalled.notify_all();
    }

    void setExtraData(std::vector<bytes> const& _extra) { m_extraData = _extra; }
    std::vector<bytes> const& extraData() const { return m_extraData; }

    /// whether should reset sealing
    /// 1. the block has been sealed
    /// 2. the unsealed block number no bigger than current block number:
    /// in the case of the next leader sealing, can't meet the condition 2,
    /// so the block generated by the next leader won't be resetted
    virtual bool shouldResetSealing()
    {
        return m_sealing.block->isSealed() ||
               m_sealing.block->blockHeader().number() <= m_blockChain->number();
    }

    /// return the pointer of ConsensusInterface to access common interfaces
    std::shared_ptr<dev::consensus::ConsensusInterface> const consensusEngine()
    {
        return m_consensusEngine;
    }

    // set consensusEngine
    virtual void setConsensusEngine(ConsensusInterface::Ptr _consensusEngine)
    {
        m_consensusEngine = _consensusEngine;
    }

    virtual void setBlockFactory(dev::eth::BlockFactory::Ptr _blockFactory)
    {
        m_sealing.setBlockFactory(_blockFactory);
        m_consensusEngine->setBlockFactory(_blockFactory);
    }

protected:
    void reportNewBlock();
    /// sealing block
    virtual bool shouldSeal();
    virtual bool shouldWait(bool const& wait) const;
    /// load transactions from transaction pool
    void loadTransactions(uint64_t const& transToFetch);
    virtual bool checkTxsEnough(uint64_t maxTxsCanSeal)
    {
        uint64_t tx_num = m_sealing.block->getTransactionSize();
        bool enough =
            canHandleBlockForNextLeader() && (tx_num >= maxTxsCanSeal || reachBlockIntervalTime());
        if (enough)
        {
            SEAL_LOG(DEBUG) << "[checkTxsEnough] Tx enough: [txNum]: " << tx_num;
        }
        return enough;
    }

    /// in case of the next leader packeted the number of maxTransNum transactions before the last
    /// block is consensused
    virtual bool canHandleBlockForNextLeader() { return true; }
    virtual bool reachBlockIntervalTime() { return false; }
    virtual void handleBlock() {}
    virtual bool shouldHandleBlock() { return true; }
    virtual void doWork(bool wait);
    void doWork() override { doWork(true); }
    bool isBlockSyncing();

    inline void resetSealingBlock(h256Hash const& filter = h256Hash(), bool resetNextLeader = false)
    {
        SEAL_LOG(DEBUG) << "[resetSealingBlock]" << LOG_KV("blkNum", m_blockChain->number())
                        << LOG_KV("sealingNum", m_sealing.block->blockHeader().number());
        m_blockSync->noteSealingBlockNumber(m_blockChain->number());
        resetSealingBlock(m_sealing, filter, resetNextLeader);
    }
    /// reset the sealing block before loadTransactions
    void resetSealingBlock(
        Sealing& sealing, h256Hash const& filter = h256Hash(), bool resetNextLeader = false);
    void resetBlock(std::shared_ptr<dev::eth::Block> block, bool resetNextLeader = false);

    /// reset the sealing Header after loadTransactions, before generate and broadcast local prepare
    /// message
    void resetSealingHeader(dev::eth::BlockHeader& header);
    /// reset timestamp of block header
    void resetCurrentTime()
    {
        int64_t parentTime =
            m_blockChain->getBlockByNumber(m_blockChain->number())->header().timestamp();
        m_sealing.block->header().setTimestamp(
            std::max(parentTime + 1, m_consensusEngine->getAlignedTime()));
    }

protected:
    uint64_t maxBlockCanSeal()
    {
        ReadGuard l(x_maxBlockCanSeal);
        return m_maxBlockCanSeal;
    }
    /// transaction pool handler
    std::shared_ptr<dev::txpool::TxPoolInterface> m_txPool;
    /// handler of the block-sync module
    std::shared_ptr<dev::sync::SyncInterface> m_blockSync;
    /// handler of the block chain module
    std::shared_ptr<dev::blockchain::BlockChainInterface> m_blockChain;
    std::shared_ptr<dev::consensus::ConsensusInterface> m_consensusEngine;

    /// current sealing block(include block, transaction set of block and execute context)
    Sealing m_sealing;
    /// lock on m_sealing
    mutable RecursiveMutex x_sealing;
    /// extra data
    std::vector<bytes> m_extraData;

    /// atomic value represents that whether is calling syncTransactionQueue now
    /// signal to notify all thread to work
    boost::condition_variable m_signalled;
    boost::condition_variable m_blockSignalled;
    /// mutex to access m_signalled
    boost::mutex x_signalled;
    boost::mutex x_blocksignalled;
    std::atomic<bool> m_syncTxPool = {false};
    /// a new block has been submitted to the blockchain
    std::atomic<bool> m_syncBlock = {false};

    ///< Has the remote worker recently been reset?
    bool m_remoteWorking = false;
    /// True if we /should/ be sealing.
    std::atomic_bool m_startConsensus = {false};

    /// handler
    Handler<> m_tqReady;
    Handler<int64_t> m_blockSubmitted;

    /// the maximum transaction number that can be sealed in a block
    uint64_t m_maxBlockCanSeal = 10000; // zd
    mutable SharedMutex x_maxBlockCanSeal;
};
}  // namespace consensus
}  // namespace dev
