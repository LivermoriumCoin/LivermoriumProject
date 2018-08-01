// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The livermorium developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "kernel.h"
#include "txdb.h"

using namespace std;

extern bool IsConfirmedInNPrevBlocks(const CTxIndex& txindex, const CBlockIndex* pindexFrom, int nMaxDepth, int& nActualDepth);

// Get time weight
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd)
{
	// Kernel hash weight starts from 0 at the min age
	// this change increases active coins participating the hash and helps
	// to secure the network when proof-of-stake difficulty is low

	return nIntervalEnd - nIntervalBeginning - nStakeMinAge;
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
	if (!pindex)
		return error("GetLastStakeModifier: null pindex");
	while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
		pindex = pindex->pprev;
	if (!pindex->GeneratedStakeModifier())
		return error("GetLastStakeModifier: no generation at genesis block");
	nStakeModifier = pindex->nStakeModifier;
	nModifierTime = pindex->GetBlockTime();
	return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
	assert(nSection >= 0 && nSection < 64);
	return (nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
	int64_t nSelectionInterval = 0;
	for (int nSection = 0; nSection < 64; nSection++)
		nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
	return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp, map<uint256, const CBlockIndex*>& mapSelectedBlocks,
	int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev, const CBlockIndex** pindexSelected)
{
	bool fSelected = false;
	uint256 hashBest = 0;
	*pindexSelected = (const CBlockIndex*)0;
	BOOST_FOREACH(const PAIRTYPE(int64_t, uint256)& item, vSortedByTimestamp)
	{
		if (!mapBlockIndex.count(item.second))
			return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString());
		const CBlockIndex* pindex = mapBlockIndex[item.second];
		if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
			break;
		if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
			continue;
		// compute the selection hash by hashing its proof-hash and the
		// previous proof-of-stake modifier
		CDataStream ss(SER_GETHASH, 0);
		ss << pindex->hashProof << nStakeModifierPrev;
		uint256 hashSelection = Hash(ss.begin(), ss.end());
		// the selection hash is divided by 2**32 so that proof-of-stake block
		// is always favored over proof-of-work block. this is to preserve
		// the energy efficiency property
		if (pindex->IsProofOfStake())
			hashSelection >>= 32;
		if (fSelected && hashSelection < hashBest)
		{
			hashBest = hashSelection;
			*pindexSelected = (const CBlockIndex*)pindex;
		}
		else if (!fSelected)
		{
			fSelected = true;
			hashBest = hashSelection;
			*pindexSelected = (const CBlockIndex*)pindex;
		}
	}
	LogPrint("stakemodifier", "SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString());
	return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
	nStakeModifier = 0;
	fGeneratedStakeModifier = false;
	if (!pindexPrev)
	{
		fGeneratedStakeModifier = true;
		return true;  // genesis block's modifier is 0
	}
	// First find current stake modifier and its generation block time
	// if it's not old enough, return the same stake modifier
	int64_t nModifierTime = 0;
	if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
		return error("ComputeNextStakeModifier: unable to get last modifier");
	LogPrint("stakemodifier", "ComputeNextStakeModifier: prev modifier=0x%016x time=%s\n", nStakeModifier, DateTimeStrFormat(nModifierTime));
	if (nModifierTime / nModifierInterval >= pindexPrev->GetBlockTime() / nModifierInterval)
		return true;

	// Sort candidate blocks by timestamp
	vector<pair<int64_t, uint256> > vSortedByTimestamp;
	vSortedByTimestamp.reserve(64 * nModifierInterval / TARGET_SPACING);

	int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
	int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / nModifierInterval) * nModifierInterval - nSelectionInterval;
	const CBlockIndex* pindex = pindexPrev;
	while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
	{
		vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
		pindex = pindex->pprev;
	}
	int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
	reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
	sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

	// Select 64 blocks from candidate blocks to generate stake modifier
	uint64_t nStakeModifierNew = 0;
	int64_t nSelectionIntervalStop = nSelectionIntervalStart;
	map<uint256, const CBlockIndex*> mapSelectedBlocks;
	for (int nRound = 0; nRound < min(64, (int)vSortedByTimestamp.size()); nRound++)
	{
		// add an interval section to the current selection round
		nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
		// select a block from the candidates of current round
		if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
			return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
		// write the entropy bit of the selected block
		nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
		// add the selected block from candidates to selected list
		mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
		LogPrint("stakemodifier", "ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n", nRound, DateTimeStrFormat(nSelectionIntervalStop), pindex->nHeight, pindex->GetStakeEntropyBit());
	}

	// Print selection map for visualization of the selected blocks
	if (LogAcceptCategory("stakemodifier"))
	{
		string strSelectionMap = "";
		// '-' indicates proof-of-work blocks not selected
		strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
		pindex = pindexPrev;
		while (pindex && pindex->nHeight >= nHeightFirstCandidate)
		{
			// '=' indicates proof-of-stake blocks not selected
			if (pindex->IsProofOfStake())
				strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
			pindex = pindex->pprev;
		}
		BOOST_FOREACH(const PAIRTYPE(uint256, const CBlockIndex*)& item, mapSelectedBlocks)
		{
			// 'S' indicates selected proof-of-stake blocks
			// 'W' indicates selected proof-of-work blocks
			strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake() ? "S" : "W");
		}
		LogPrintf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap);
	}
	LogPrint("stakemodifier", "ComputeNextStakeModifier: new modifier=0x%016x time=%s\n", nStakeModifierNew, DateTimeStrFormat(pindexPrev->GetBlockTime()));

	nStakeModifier = nStakeModifierNew;
	fGeneratedStakeModifier = true;
	return true;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifierV2(const CBlockIndex* pindexPrev, const uint256& kernel)
{
	if (!pindexPrev)
		return 0;  // genesis block's modifier is 0

	CDataStream ss(SER_GETHASH, 0);
	ss << kernel << pindexPrev->bnStakeModifierV2;
	return Hash(ss.begin(), ss.end());
}

bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, unsigned int nTimeBlockFrom, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
	if (nTimeTx < txPrev.nTime)  // Transaction timestamp violation
		return error("CheckStakeKernelHash() : nTime violation");


	if (nTimeBlockFrom + nStakeMinAge > nTimeTx) // Min age requirement
		return error("CheckStakeKernelHash() : min age violation");
	//if (nTimeBlockFrom + nStakeMaxAge < nTimeTx) // Max age requirement
		//return error("CheckStakeKernelHash() : max age violation");




	// Base target
	CBigNum bnTarget;
	bnTarget.SetCompact(nBits);

	// Weighted target
	int64_t nValueIn = txPrev.vout[prevout.n].nValue;
	CBigNum bnWeight = CBigNum(nValueIn);
	bnTarget *= bnWeight;

	targetProofOfStake = bnTarget.getuint256();

	uint64_t nStakeModifier = pindexPrev->nStakeModifier;
	uint256 bnStakeModifierV2 = pindexPrev->bnStakeModifierV2;
	int nStakeModifierHeight = pindexPrev->nHeight;
	int64_t nStakeModifierTime = pindexPrev->nTime;

	// Calculate hash
	CDataStream ss(SER_GETHASH, 0);

	ss << nStakeModifier << nTimeBlockFrom << txPrev.nTime << prevout.hash << prevout.n << nTimeTx;
	hashProofOfStake = Hash(ss.begin(), ss.end());

	if (fPrintProofOfStake)
	{
		LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from timestamp=%s\n",
			nStakeModifier, nStakeModifierHeight,
			DateTimeStrFormat(nStakeModifierTime),
			DateTimeStrFormat(nTimeBlockFrom));
		LogPrintf("CheckStakeKernelHash() : check modifier=0x%016x nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
			nStakeModifier,
			nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
			hashProofOfStake.ToString());
	}

	// Now check if proof-of-stake hash meets target protocol
	if (CBigNum(hashProofOfStake) > bnTarget) {
		return false;
	}

	if (fDebug && !fPrintProofOfStake)
	{
		LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from timestamp=%s\n",
			nStakeModifier, nStakeModifierHeight,
			DateTimeStrFormat(nStakeModifierTime),
			DateTimeStrFormat(nTimeBlockFrom));
		LogPrintf("CheckStakeKernelHash() : pass modifier=0x%016x nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
			nStakeModifier,
			nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
			hashProofOfStake.ToString());
	}

	return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, uint256& hashProofOfStake, uint256& targetProofOfStake)
{

	int nStakeMinConfirmations = 15;
	if (!tx.IsCoinStake())
		return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

	// Kernel (input 0) must match the stake hash target per coin age (nBits)
	const CTxIn& txin = tx.vin[0];

	// First try finding the previous transaction in database
	CTxDB txdb("r");
	CTransaction txPrev;
	CTxIndex txindex;
	if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
		return tx.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download

	// Verify signature
	if (!VerifySignature(txPrev, tx, 0, SCRIPT_VERIFY_NONE, 0))
		return tx.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

	// Read block header
	CBlock block;
	if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
		return fDebug ? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction

	if (!CheckStakeKernelHash(pindexPrev, nBits, block.GetBlockTime(), txPrev, txin.prevout, tx.nTime, hashProofOfStake, targetProofOfStake, fDebug))
		return tx.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

	return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int nHeight, int64_t nTimeBlock, int64_t nTimeTx)
{
	return (nTimeBlock == nTimeTx) && ((nTimeTx & STAKE_TIMESTAMP_MASK) == 0);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, int64_t nTime, const COutPoint& prevout, int64_t* pBlockTime)
{
	uint256 hashProofOfStake, targetProofOfStake;

	CTxDB txdb("r");
	CTransaction txPrev;
	CTxIndex txindex;
	if (!txPrev.ReadFromDisk(txdb, prevout, txindex))
		return false;

	// Read block header
	CBlock block;
	if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
		return false;

	if ((block.GetBlockTime() + nStakeMinAge > nTime))// || (block.GetBlockTime() + nStakeMaxAge < nTime))
		return false; // only count coins meeting min age requirement

	int nDepth;
	int nStakeMinConfirmations = 15;
	if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfirmations - 1, nDepth))
		return false;

	if (pBlockTime)
		*pBlockTime = block.GetBlockTime();

	return CheckStakeKernelHash(pindexPrev, nBits, block.GetBlockTime(), txPrev, prevout, nTime, hashProofOfStake, targetProofOfStake);
}
