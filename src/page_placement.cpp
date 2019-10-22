#include "page_placement.h"
#include "mc.h"
#include <stdlib.h>
#include <iostream>

void
PagePlacementPolicy::initialize(Config & config)
{
	_num_chunks = _mc->getNumSets();
	_chunks = (ChunkInfo *) gm_malloc(sizeof(ChunkInfo) *  _num_chunks);

	_scheme = _mc->getScheme();
	_sample_rate = config.get<double>("sys.mem.mcdram.sampleRate");
	_miss_rate_threshold = config.get<double>("sys.mem.mcdram.MissRateThreshold", 0.3);
    _enable_replace = config.get<bool>("sys.mem.mcdram.enableReplace", true);
	_in_way_replace = config.get<bool>("sys.mem.mcdram.inWayReplace", true);

	if (_sample_rate < 1) {
		_max_count_size = 31; //g_max_count_size;
		if (_mc->getGranularity() > 4096) // large page
			_max_count_size = 255;
	} else
		_max_count_size = 255;

	_num_entries_per_chunk = 9; //g_num_entries_per_chunk;
	//_num_stable_entries = _num_entries_per_chunk / 2;
	assert(_num_entries_per_chunk > _mc->getNumWays());
	for (uint64_t i = 0; i < _num_chunks; i++)
	{
		_chunks[i].num_hits = 0;
		_chunks[i].num_misses = 0;
		_chunks[i].entries = (ChunkEntry *) gm_malloc(sizeof(ChunkEntry) * _num_entries_per_chunk);
		for (uint32_t j = 0; j < _num_entries_per_chunk; j++)
        {
			_chunks[i].entries[j].primeTagValid = false;
			_chunks[i].entries[j].subTagValid = false;
        }
	}
	_histogram = NULL;
	srand48_r(rand(), &_buffer);
	clearStats();

	g_string scheme = config.get<const char *>("sys.mem.mcdram.placementPolicy");
	primeTag_lru_bits = (uint32_t **) gm_malloc(sizeof(uint32_t *) * _mc->getNumSets());
	subTag_lru_bits = (uint32_t **) gm_malloc(sizeof(uint32_t *) * _mc->getNumSets());
	for (uint64_t i = 0; i < _mc->getNumSets(); i++) {
		primeTag_lru_bits[i] = (uint32_t *) gm_malloc(sizeof(uint32_t) * _mc->getNumWays());
		subTag_lru_bits[i] = (uint32_t *) gm_malloc(sizeof(uint32_t) * _mc->getNumWays());
		for (uint32_t j = 0; j < _mc->getNumWays(); j++)
        {
			primeTag_lru_bits[i][j] = j;
			subTag_lru_bits[i][j] = j;
        }
	}

	if (scheme == "LRU")
		_placement_policy = LRU;
	else if (scheme == "FBR")
		_placement_policy = FBR;
	else
		assert(false);
}

/*
 * primeTag | subTag | candidate | handle                               | function
 *  miss    |  miss  |  miss     | choose a random victim in candidate  | handleCacheMiss()
 *  miss    |  miss  |  hit      | swap candidate and primeTag if needed| handleCacheMiss()
 *-----------------------------------------------------------------------------------------
 *  miss    |  hit   |  miss     | swap subTag and primeTag if needed   | handleCacheHit()
 *  hit     |  miss  |  miss     | only update LRU/FBR                  | handleCacheHit()
 */

uint32_t PagePlacementPolicy::handleCacheMiss(Address tag, Address full_addr, ReqType type, uint64_t set_num, Set * set, bool &counter_access)
{
	uint64_t chunk_num = set_num;
	//ChunkInfo * chunk = &_chunks[chunk_num];
	_chunks[chunk_num].num_misses ++;
    trace(MC, "dealing with tag: 0x%lx", tag);

	if (_placement_policy == LRU)
	{
		if (!_enable_replace)
		{
			return _mc->getNumWays();
		}
		if (set->primeTagHasEmptyWay()) {
            trace(MC, "has empty way");
			updateLRU(set_num, set->primeTagGetEmptyWay(), true);
			return set->primeTagGetEmptyWay();
	 	}

	  	double f;
	  	int64_t way;
		drand48_r(&_buffer, &f);
	  	lrand48_r(&_buffer, &way);
		if (f < _sample_rate)
		{
			for (uint32_t i = 0; i < _mc->getNumWays(); i++)
			{
				if (primeTag_lru_bits[set_num][i] == _mc->getNumWays() - 1)
				{
					Address victim_tag = set->ways[i].primeTag;
					if (_mc->getTagBuffer()->canInsert(tag, victim_tag))
					{
						updateLRU(set_num, i, true);
						return i;
					} else
						return _mc->getNumWays();
				}
			}
		}
		else
			return _mc->getNumWays();
	}

	assert(_placement_policy == FBR);
	assert(_enable_replace);
#if 1
	ChunkInfo * chunk = &_chunks[chunk_num];
	// for DEBUG
	for (uint32_t way = 0; way < _mc->getNumWays(); way++)
	{
		if (set->ways[way].primeTagValidBits)
		{
			// the first few(way count) entries in chunk->entries must be the same with dram cache
			if (set->ways[way].primeTag != _chunks[chunk_num].entries[way].primeTag)
			{
				for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
					printf("ID=%d, tag=%ld, valid=%d, count=%d\n",
						i, chunk->entries[i].primeTag, chunk->entries[i].primeTagValid, chunk->entries[i].primeCount);
				for (uint32_t i = 0; i < _mc->getNumWays(); i++)
					printf("ID=%dm tag=%ld\n", i, set->ways[i].primeTag);
			}
			assert(set->ways[way].primeTag == _chunks[chunk_num].entries[way].primeTag);
		}
	}
	for (uint32_t way = _mc->getNumWays(); way< _num_entries_per_chunk; way++)
	{
		//currently we only use primeTag in candidate ways
		assert(chunk->entries[way].subTagValid == false);
    }
#endif
	// for LongCache, never replace for store (LLC dirty evict)
	if (type == STORE)
	{
        trace(MC, "Current request type is store, return");
		return _mc->getNumWays();
	}

	double sample_rate = _sample_rate;
	bool miss_rate_tune = true; //false;
	if (sample_rate == 1)
		miss_rate_tune = false;
	if (_mc->getNumRequests() < _mc->getNumSets() * _mc->getNumWays() * 64 * 8)
		sample_rate = 1;

	// the set uses FBR replacement policy
	bool updateFBR = set->primeTagHasEmptyWay() ||  sampleOrNot(sample_rate, miss_rate_tune);
    trace(MC, "primeTagGetEmptyWay return %d", set->primeTagGetEmptyWay());
	if (updateFBR)
	{
		uint32_t empty_way = set->primeTagGetEmptyWay();
        trace(MC, "empty_way =%d", empty_way);
		counter_access = true;
		_num_counter_read ++;
		_num_counter_write ++;
		uint32_t idx = getChunkEntry(tag, &_chunks[chunk_num], true);
         trace(MC, "idx = %d", idx);
		if (idx == _num_entries_per_chunk)
			return _mc->getNumWays();
		ChunkEntry * chunk_entry = &_chunks[chunk_num].entries[idx];
		chunk_entry->primeCount ++;
		if (chunk_entry->primeCount >= _max_count_size)
			handleCounterOverflow(&_chunks[chunk_num], chunk_entry, true);

		//idx = adjustEntryOrder(&_chunks[chunk_num], idx);
		//chunk_entry = &_chunks[chunk_num].entries[idx];

		// empty slots left in dram cache
		if (empty_way < _mc->getNumWays()) {
			assert(idx == empty_way);
            trace(MC, "idx:%d == empty_way:%d", idx, empty_way);
			_num_emptyPrimeTag_replace ++;
			return empty_way;
		}
		else // figure if we can replace an entry.
		{
			assert(idx >= _mc->getNumWays());
			uint32_t victim_way = pickVictimPrimeTagWay(&_chunks[chunk_num]);
			assert(victim_way < _mc->getNumWays());
/*			if (compareCounter(&_chunks[chunk_num].entries[idx], &_chunks[chunk_num].entries[victim_way]) && !_mc->getTagBuffer()->canInsert(tag, _chunks[chunk_num].entries[victim_way].tag))
			{
				printf("!!!!!!Occupancy = %f\n", _mc->getTagBuffer()->getOccupancy());
				static int n = 0;
				printf("cannot insert (%d)   occupancy=%f.  set1=%ld, set2=%ld\n",
						n++, _mc->getTagBuffer()->getOccupancy(), (tag % 128), _chunks[chunk_num].entries[victim_way].tag % 128);
			}
*/
			if (compareCandidatePrimeCounter(&_chunks[chunk_num].entries[idx], &_chunks[chunk_num].entries[victim_way])
				&& _mc->getTagBuffer()->canInsert(tag, _chunks[chunk_num].entries[victim_way].primeTag))
			{
				//assert(idx < _num_stable_entries);
				// swap current way with victim way.
				ChunkEntry tmp = _chunks[chunk_num].entries[idx];
				_chunks[chunk_num].entries[idx] = _chunks[chunk_num].entries[victim_way];
				_chunks[chunk_num].entries[victim_way] = tmp;
				//assert(idx >= _mc->getNumWays() && idx < _num_stable_entries);
				return victim_way;
			}
			else {
				return _mc->getNumWays();
			}
		}
	}
    return _mc->getNumWays();
}

/*
 * primeTag | subTag | candidate | handle                               | function
 *  miss    |  miss  |  miss     | choose a random victim in candidate  | handleCacheMiss()
 *  miss    |  miss  |  hit      | swap candidate and primeTag if needed| handleCacheMiss()
 *-----------------------------------------------------------------------------------------
 *  miss    |  hit   |  miss     | swap subTag and primeTag if needed   | handleCacheHit()
 *  hit     |  miss  |  miss     | only update LRU/FBR                  | handleCacheHit()
 *
 * return the victim way of primeTag way which will be swaped with subTag which is hit
 */
uint32_t PagePlacementPolicy::handleCacheHit(Address tag, Address full_addr, ReqType type, uint64_t set_num, Set * set, bool &counter_access, uint32_t hit_way)
{
	assert(tag == set->ways[hit_way].primeTag || tag == set->ways[hit_way].subTag);

	bool hitPrimeTag=false;
	if (tag == set->ways[hit_way].primeTag)
	{
		hitPrimeTag = true;
	}
	if (_placement_policy == LRU)
	{
		updateLRU(set_num, hit_way, hitPrimeTag);
		return _mc->getNumWays();
	}
	uint64_t chunk_num = set_num;
	ChunkInfo * chunk = &_chunks[chunk_num];

	if(hitPrimeTag)
	{
        trace(MC, "Hit primeTag in way %d!", hit_way);
#if 1
		// for DEBUG
		for (uint32_t way = 0; way < _mc->getNumWays(); way++)
		{
			if (set->ways[way].primeTagValidBits)
			{
				// the first few(way count) entries in chunk->entries must be the same with dram cache
				if (set->ways[way].primeTag != _chunks[chunk_num].entries[way].primeTag)
				{
					for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
						printf("ID=%d, tag=%ld, valid=%d, count=%d\n",
							i, chunk->entries[i].primeTag, chunk->entries[i].primeTagValid, chunk->entries[i].primeCount);
					for (uint32_t i = 0; i < _mc->getNumWays(); i++)
						printf("ID=%dm tag=%ld\n", i, set->ways[i].primeTag);
				}
				assert(set->ways[way].primeTag == _chunks[chunk_num].entries[way].primeTag);
			}
		}
	for (uint32_t way = _mc->getNumWays(); way< _num_entries_per_chunk; way++)
	{
		//currently we only use primeTag in candidate ways
		assert(chunk->entries[way].subTagValid == false);
    }
#endif
		assert(_placement_policy == FBR);

		double sample_rate = _sample_rate;
		bool miss_rate_tune = true;
		if (sample_rate == 1)
			miss_rate_tune = false;
		if (_mc->getNumRequests() < _mc->getNumSets() * _mc->getNumWays() * 64 * 8)
		 	sample_rate = 1;
		if (sampleOrNot(sample_rate, miss_rate_tune))
		{
			counter_access = true;
			_num_counter_read ++;
			_num_counter_write ++;
			uint32_t idx = getChunkEntry(tag, &_chunks[chunk_num], hitPrimeTag);
            trace(MC, "Cache Hit in primeTag, chunk entry idx = %d", idx);
			ChunkEntry * chunk_entry = &_chunks[chunk_num].entries[idx];
			// cache hit, so idx must belong to the normal ways, not in the candidate ways
			assert(idx < _mc->getNumWays());
			chunk_entry->primeCount++;
			//assert( idx == adjustEntryOrder(&_chunks[chunk_num], idx ));
			if (chunk_entry->primeCount >= _max_count_size)
				handleCounterOverflow(&_chunks[chunk_num], chunk_entry, hitPrimeTag);
		}
	}
	else // hit subTag, update FBR and maybe need swap primeTag/subTag(just like victim cache hit)
	{
#if 1
		// for DEBUG
		// the first few entries in chunk->entries must be in dram cache
		for (uint32_t way = 0; way < _mc->getNumWays(); way++)
		{
			if (set->ways[way].subTagValidBits)
			{
				if (set->ways[way].subTag != _chunks[chunk_num].entries[way].subTag)
				{
					for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
						printf("ID=%d, tag=%ld, valid=%d, count=%d\n",
							i, chunk->entries[i].subTag, chunk->entries[i].subTagValid, chunk->entries[i].subCount);
					for (uint32_t i = 0; i < _mc->getNumWays(); i++)
						printf("ID=%dm tag=%ld\n", i, set->ways[i].subTag);
				}
				assert(set->ways[way].subTag == _chunks[chunk_num].entries[way].subTag);
			}
		}
#endif
		assert(tag == set->ways[hit_way].subTag);

		double sample_rate = _sample_rate;
		bool miss_rate_tune = true; //false;
		if (sample_rate == 1)
			miss_rate_tune = false;
		if (_mc->getNumRequests() < _mc->getNumSets() * _mc->getNumWays() * 64 * 8)
			sample_rate = 1;

		// Based on FBR replacement policy, when hit subTag we may need swap subTag and primeTag
		// or when primeTag has empty way we can promote subTag immediatly
		bool updateFBR = sampleOrNot(sample_rate, miss_rate_tune) || set->primeTagHasEmptyWay();
		if (updateFBR)
		{
			counter_access = true;
			_num_counter_read ++;
			_num_counter_write ++;
			uint32_t idx = getChunkEntry(tag, &_chunks[chunk_num], hitPrimeTag);
            trace(MC, "Cache Hit in subTag, chunk entry idx = %d", idx);
			ChunkEntry * chunk_entry = &_chunks[chunk_num].entries[idx];
			// subTag cache hit, so idx must belong to the normal ways, not in the candidate ways
			assert(idx < _mc->getNumWays());
			chunk_entry->subCount++;

			if (chunk_entry->subCount >= _max_count_size)
				handleCounterOverflow(&_chunks[chunk_num], chunk_entry, hitPrimeTag);

			//if there is empty slot left in dram cache's primeTag, swap it
			uint32_t primeTag_empty_way = set->primeTagGetEmptyWay();
			if (primeTag_empty_way < _mc->getNumWays())
			{
                trace(MC, "primeTag has empty way:%d", primeTag_empty_way);
				assert(idx == primeTag_empty_way);
				_num_subTag_promote ++;
				// we definitely can replace the empty way with subTag
				// empty_way is already filled in getChunkEntry(), no need to handle here
				return primeTag_empty_way;
			}
			else
			{
                trace(MC, "primeTag DON'T has empty way");
				// no empty way in primeTag when subTag hit,
				// figure out if we can replace a primeTag way and do subTag promote:
				// if subTag's count is larger than min primeTag + threshold, swap them
				assert(idx < _mc->getNumWays());
				uint32_t victim_way = pickVictimPrimeTagWay(&_chunks[chunk_num]);
                trace(MC, "Picking a Victim way: %d", victim_way);
				assert(victim_way < _mc->getNumWays());

				if (compareSubPrimeCounter(&_chunks[chunk_num].entries[idx], &_chunks[chunk_num].entries[victim_way])
					&& _mc->getTagBuffer()->canInsert(tag, _chunks[chunk_num].entries[victim_way].primeTag))
				{
                    trace(MC, "swaping current hit subTag:%d way with primeTag way:%d", hit_way, idx);
					//assert(idx < _num_stable_entries);
					// swap current way with victim way.
					ChunkEntry tmp = _chunks[chunk_num].entries[idx];
					_chunks[chunk_num].entries[idx] = _chunks[chunk_num].entries[victim_way];
					_chunks[chunk_num].entries[victim_way] = tmp;
					//assert(idx >= _mc->getNumWays() && idx < _num_stable_entries);
					return victim_way;
				}
				else {
                    trace(MC, "Although has empty primeTag way, no swaping");
					return _mc->getNumWays();
				}
			}
		}
	}
    // shouldn't get here, DramCache hit only happen in primeTag or subTag
    assert("false");
    return _mc->getNumWays();
}

/*
 * insert primeTag's victim into subTag,
 * return subTag's victim way
 *  replaced_primeTag_way: valid   replaced_subTag_way: valid      candidate_way
 *  replaced_primeTag_way: valid   replaced_subTag_way: invalid    candidate_way
 *  replaced_primeTag_way: invalid   replaced_subTag_way: valid    candidate_way
 *  replaced_primeTag_way: invalid   replaced_subTag_way: invalid  candidate_way
 * TODO: this function is not correct. the flow primeTag->subTag->candidate is not finished
 * !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 */
uint32_t PagePlacementPolicy::handlePrimeTagEvict(uint32_t replaced_primeTag_way, Address tag, ReqType type, uint64_t set_num, Set * set, bool &counter_access)
{
	uint64_t chunk_num = set_num;
	//ChunkInfo * chunk = &_chunks[chunk_num];
	_chunks[chunk_num].num_misses ++;
    trace(MC, "dealing with tag: 0x%lx", tag);
    if(_in_way_replace)
        return replaced_primeTag_way;

	if (_placement_policy == LRU)
	{
		if (!_enable_replace)
		{
			return _mc->getNumWays();
		}
		if (set->subTagHasEmptyWay()) {
            trace(MC, "has empty way");
			updateLRU(set_num, set->subTagGetEmptyWay(), false);
			return set->subTagGetEmptyWay();
	 	}

	  	double f;
	  	int64_t way;
		drand48_r(&_buffer, &f);
	  	lrand48_r(&_buffer, &way);
		if (f < _sample_rate)
		{
			for (uint32_t i = 0; i < _mc->getNumWays(); i++)
			{
				if (subTag_lru_bits[set_num][i] == _mc->getNumWays() - 1)
				{
					Address victim_tag = set->ways[i].subTag;
					if (_mc->getTagBuffer()->canInsert(tag, victim_tag))
					{
						updateLRU(set_num, i, true);
						return i;
					} else
						return _mc->getNumWays();
				}
			}
		}
		else
			return _mc->getNumWays();
	}

	assert(_placement_policy == FBR);
	assert(_enable_replace);
#if 1
	ChunkInfo * chunk = &_chunks[chunk_num];
	// for DEBUG
	for (uint32_t way = 0; way < _mc->getNumWays(); way++)
	{
		if (set->ways[way].subTagValidBits)
		{
			// the first few(way count) entries in chunk->entries must be the same with dram cache
			if (set->ways[way].subTag != _chunks[chunk_num].entries[way].subTag)
			{
				for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
					printf("ID=%d, tag=%ld, valid=%d, count=%d\n",
						i, chunk->entries[i].subTag, chunk->entries[i].subTagValid, chunk->entries[i].subCount);
				for (uint32_t i = 0; i < _mc->getNumWays(); i++)
					printf("ID=%dm tag=%ld\n", i, set->ways[i].subTag);
			}
			assert(set->ways[way].subTag == _chunks[chunk_num].entries[way].subTag);
		}
	}
	for (uint32_t way = _mc->getNumWays(); way< _num_entries_per_chunk; way++)
	{
		//currently we only use subTag in candidate ways
		assert(chunk->entries[way].subTagValid == false);
    }
#endif

	// for LongCache, never replace for store (LLC dirty evict)
	if (type == STORE)
	{
		return _mc->getNumWays();
	}

	double sample_rate = _sample_rate;
	bool miss_rate_tune = true; //false;
	if (sample_rate == 1)
		miss_rate_tune = false;
	if (_mc->getNumRequests() < _mc->getNumSets() * _mc->getNumWays() * 64 * 8)
		sample_rate = 1;

	// the set uses FBR replacement policy
	bool updateFBR = set->subTagHasEmptyWay() ||  sampleOrNot(sample_rate, miss_rate_tune);
	if (updateFBR)
	{
		uint32_t empty_way = set->subTagGetEmptyWay();
        trace(MC, "subTag empty_way =%d", empty_way);
		counter_access = true;
		_num_counter_read ++;
		_num_counter_write ++;
		uint32_t idx = getChunkEntry(tag, &_chunks[chunk_num], false);
        trace(MC, "subTag idx = %d", idx);
		if (idx == _num_entries_per_chunk)
			return _mc->getNumWays();
		ChunkEntry * chunk_entry = &_chunks[chunk_num].entries[idx];
		chunk_entry->subCount ++;
		if (chunk_entry->subCount >= _max_count_size)
			handleCounterOverflow(&_chunks[chunk_num], chunk_entry, false);

		//idx = adjustEntryOrder(&_chunks[chunk_num], idx);
		//chunk_entry = &_chunks[chunk_num].entries[idx];

		// empty slots left in dram cache
		if (empty_way < _mc->getNumWays()) {
			assert(idx == empty_way);
            trace(MC, "idx:%d == empty_way:%d", idx, empty_way);
			_num_emptySubTag_replace ++;
			return empty_way;
		}
		else // figure if we can replace an entry.
		{
			assert(idx >= _mc->getNumWays());
			uint32_t victim_way = pickVictimSubTagWay(&_chunks[chunk_num]);
			assert(victim_way < _mc->getNumWays());
/*			if (compareCounter(&_chunks[chunk_num].entries[idx], &_chunks[chunk_num].entries[victim_way]) && !_mc->getTagBuffer()->canInsert(tag, _chunks[chunk_num].entries[victim_way].tag))
			{
				printf("!!!!!!Occupancy = %f\n", _mc->getTagBuffer()->getOccupancy());
				static int n = 0;
				printf("cannot insert (%d)   occupancy=%f.  set1=%ld, set2=%ld\n",
						n++, _mc->getTagBuffer()->getOccupancy(), (tag % 128), _chunks[chunk_num].entries[victim_way].tag % 128);
			}
*/
			if (compareCandidatePrimeCounter(&_chunks[chunk_num].entries[idx], &_chunks[chunk_num].entries[victim_way])
				&& _mc->getTagBuffer()->canInsert(tag, _chunks[chunk_num].entries[victim_way].primeTag))
			{
				//assert(idx < _num_stable_entries);
				// swap current way with victim way.
				ChunkEntry tmp = _chunks[chunk_num].entries[idx];
				_chunks[chunk_num].entries[idx] = _chunks[chunk_num].entries[victim_way];
				_chunks[chunk_num].entries[victim_way] = tmp;
				//assert(idx >= _mc->getNumWays() && idx < _num_stable_entries);
				return victim_way;
			}
			else {
				return _mc->getNumWays();
			}
		}
	}
    return _mc->getNumWays();
}
#if 0
uint32_t PagePlacementPolicy::handleSubTagHit(uint32_t replaced_primeTag_way, Address tag, Address full_addr, ReqType type, uint64_t set_num, Set * set, bool &counter_access)
{
	if(_in_way_replace)
	{
		return replaced_primeTag_way;
	}
}
#endif

/*
 * 1. if called by handleCacheHit( maybe hit primeTag/subTag ) in primeTag/subTag,
 *    return the hit way num,so we should pass isPrimeTag to this function
 * 2. if called by handleCacheMiss(), return invalid primeTag or candidate primeTag way num,
 *    called with isPrimeTag set
 * 3. if called by handlePrimeTagEvict(),
 *    pickVictimSubTagWay() will return subTag victim, here only return invalid subTag
 */
uint32_t PagePlacementPolicy::getChunkEntry(Address tag, ChunkInfo * chunk_info, bool allocate, bool isPrimeTag)
{
	uint32_t idx = _num_entries_per_chunk;
	if (isPrimeTag)
	{
		// if hit, return the primeTag hit way num
		for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
		{
			if (chunk_info->entries[i].primeTagValid && chunk_info->entries[i].primeTag == tag)
				return i; //&chunk_info->entries[i];
			// if miss, find the first invalid way in primeTag and candidate, and replace it
			else if (!chunk_info->entries[i].primeTagValid && idx == _num_entries_per_chunk)
				idx = i;
		}
	}
	// there is no miss handle in subTag, if miss both in primeTag and subTag,
	// allocation should happen in primeTag.
	else
	{
		// if hit, return the subTag hit way num
		for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
		{
			if (chunk_info->entries[i].subTagValid && chunk_info->entries[i].subTag == tag)
				return i; //&chunk_info->entries[i];
		}
		// if we dealing with subTag, it must be hit, because subTag act as victim
		assert(false);
	}

	// miss in primeTag/subTag/candidate,
	// no invalid entry to replace, find one in candidate
	if (idx == _num_entries_per_chunk && allocate)
	{
	  	int64_t rand;
		double f;
	  	lrand48_r(&_buffer, &rand);
		drand48_r(&_buffer, &f);
		// randomly pick a victim entry
		idx = _mc->getNumWays() + rand % (_num_entries_per_chunk - _mc->getNumWays());
		assert(idx >= _mc->getNumWays());
		// replace the entry with certain probability.
		// high count value reduces the probability
		if (chunk_info->entries[idx].primeCount > 0 && f > 1.0 / chunk_info->entries[idx].primeCount)
			idx = _num_entries_per_chunk;
	}
	// replace the invalid primeTag or victim in candidate
	if (idx < _num_entries_per_chunk) {
		chunk_info->entries[idx].primeTagValid = true;
		chunk_info->entries[idx].primeTag = tag;
		chunk_info->entries[idx].primeCount = 0;
	}
	return idx;
}

bool
PagePlacementPolicy::sampleOrNot(double sample_rate, bool miss_rate_tune)
{
	double miss_rate = _mc->getRecentMissRate();
	double f;
	drand48_r(&_buffer, &f);
	if (miss_rate_tune)
		return f < sample_rate * miss_rate;
	else
		return f < sample_rate;
}


// compare subTag and primeTag count
// entry1 is subTag, entry2 is primeTag
bool PagePlacementPolicy::compareSubPrimeCounter(ChunkEntry * entry1, ChunkEntry * entry2)
{

	if (!entry1)
		return false;
	else if (!entry2)
		return entry1->subCount > 0;
	else
		return entry1->subCount >= entry2->primeCount + (_mc->getGranularity() / 64 / 2) * _sample_rate;
}

// compare candidate and primeTag count
// entry1 is candidate, entry2 is primeTag
bool PagePlacementPolicy::compareCandidatePrimeCounter(ChunkEntry * entry1, ChunkEntry * entry2)
{
		if (!entry1)
			return false;
		else if (!entry2)
			return entry1->primeCount > 0;
		else
			return entry1->primeCount >= entry2->primeCount + (_mc->getGranularity() / 64 / 2) * _sample_rate;
}


uint32_t
PagePlacementPolicy::adjustEntryOrder(ChunkInfo * chunk_info, uint32_t idx)
{
	assert(false);
/*
	// the modified entry was a stable entry, return
	if (idx < _num_stable_entries)
		return idx;
	// the modified entry may be promoted.
	uint32_t min_count = 10000;
	uint32_t min_idx = 100;
	for (uint32_t i = _mc->getNumWays(); i < _num_stable_entries; i++)
	{
		assert(chunk_info->entries[i].valid);
		if (chunk_info->entries[i].count < min_count)
		{
			min_count = chunk_info->entries[i].count;
			min_idx = i;
		}
	}
	if (min_count <= chunk_info->entries[idx].count)
	{
		// swap the two entries
		ChunkEntry tmp = chunk_info->entries[idx];
		chunk_info->entries[idx] = chunk_info->entries[min_idx];
		chunk_info->entries[min_idx] = tmp;
		return min_idx;
	}
	return idx;
*/
}

uint32_t  PagePlacementPolicy::pickVictimPrimeTagWay(ChunkInfo * chunk_info)
{
	uint32_t min_count = 10000;
	uint32_t min_idx = _mc->getNumWays();
	for (uint32_t i = 0; i < _mc->getNumWays(); i++)
	{
		assert(chunk_info->entries[i].primeTagValid);
		if (chunk_info->entries[i].primeCount < min_count)
		{
			min_count = chunk_info->entries[i].primeCount;
			min_idx = i;
		}
	}
	return min_idx;
}
uint32_t  PagePlacementPolicy::pickVictimSubTagWay(ChunkInfo * chunk_info)
{
	uint32_t min_count = 10000;
	uint32_t min_idx = _mc->getNumWays();
	for (uint32_t i = 0; i < _mc->getNumWays(); i++)
	{
		assert(chunk_info->entries[i].subTagValid);
		if (chunk_info->entries[i].subCount < min_count)
		{
			min_count = chunk_info->entries[i].subCount;
			min_idx = i;
		}
	}
	return min_idx;
}


void
PagePlacementPolicy::handleCounterOverflow(ChunkInfo * chunk_info, ChunkEntry * overflow_entry, bool isPrimeTag)
{
	for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
	{
		if(isPrimeTag)
		{
			if (overflow_entry == &chunk_info->entries[i])
				++(overflow_entry->primeCount) /= 2;
			else
				chunk_info->entries[i].primeCount /= 2;
		}
		else
		{
			if (overflow_entry == &chunk_info->entries[i])
				++(overflow_entry->subCount) /= 2;
			else
				chunk_info->entries[i].subCount /= 2;
		}

	}
}

void
PagePlacementPolicy::clearStats()
{
	_num_counter_read = 0;
	_num_counter_write = 0;

	_num_emptyPrimeTag_replace = 0;
	_num_emptySubTag_replace = 0;
	_num_subTag_promote = 0;
}


#if 0
void
PagePlacementPolicy::computeFreqDistr()
{
	assert(!_histogram);
	//_histogram = new uint64_t [_num_entries_per_chunk];
	for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
		_histogram[i] = 0;
	for (uint64_t chunk_id = 0; chunk_id < _num_chunks; chunk_id ++)
	{
		// pick the most frequent chunk, then the second most frequent, etc.
		for (uint32_t i = 0; i < _num_entries_per_chunk; i++)
		{
			uint32_t idx = _num_entries_per_chunk;
			uint32_t max_count = 0;
			for (uint32_t j = 0; j < _num_entries_per_chunk; j++)
			{
				if (_chunks[chunk_id].entries[j].valid)
				{
					if (_chunks[chunk_id].entries[j].count > max_count)
					{
						max_count = _chunks[chunk_id].entries[j].count;
						idx = j;
					}
				}
				else
					break;
			}
			if (idx != _num_entries_per_chunk)
			{
				_histogram[i] += max_count;
				_chunks[chunk_id].entries[idx].count = 0;
			}
			else
				break;
		}
	}
}
#endif

void
PagePlacementPolicy::updateLRU(uint64_t set_num, uint32_t way_num, bool isPrimeTag)
{
	if(isPrimeTag)
	{
		for (uint32_t i = 0; i < _mc->getNumWays(); i++)
			if (primeTag_lru_bits[set_num][i] < primeTag_lru_bits[set_num][way_num])
				primeTag_lru_bits[set_num][i] ++;
		primeTag_lru_bits[set_num][way_num] = 0;
	}
	else
	{
		for (uint32_t i = 0; i < _mc->getNumWays(); i++)
			if (subTag_lru_bits[set_num][i] < subTag_lru_bits[set_num][way_num])
				subTag_lru_bits[set_num][i] ++;
		subTag_lru_bits[set_num][way_num] = 0;
	}
}

void
PagePlacementPolicy::flushChunk(uint32_t set)
{
	for (uint32_t i = 0; i < _num_entries_per_chunk; i ++) {
		_chunks[set].entries[i].primeTagValid = false;
		_chunks[set].entries[i].primeTag = 0;
		_chunks[set].entries[i].primeCount = 0;

		_chunks[set].entries[i].subTagValid = false;
		_chunks[set].entries[i].subTag = 0;
		_chunks[set].entries[i].subCount = 0;
	}
}

