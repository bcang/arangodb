////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "ModificationExecutorTraits.h"
#include "Aql/AqlValue.h"
#include "Aql/Collection.h"
#include "Aql/OutputAqlItemRow.h"
#include "Basics/Common.h"
#include "Cluster/ServerState.h"
#include "VocBase/LogicalCollection.h"

#include <algorithm>

#include "velocypack/Collection.h"
#include "velocypack/velocypack-aliases.h"

using namespace arangodb;
using namespace arangodb::aql;

namespace {

int extractKeyAndRev(transaction::Methods* trx, AqlValue const& value,
                     std::string& key, std::string& rev, bool keyOnly = false) {
  if (value.isObject()) {
    bool mustDestroy;
    auto resolver = trx->resolver();
    TRI_ASSERT(resolver != nullptr);
    AqlValue sub = value.get(*resolver, StaticStrings::KeyString, mustDestroy, false);
    AqlValueGuard guard(sub, mustDestroy);

    if (sub.isString()) {
      key.assign(sub.slice().copyString());

      if (!keyOnly) {
        bool mustDestroyToo;
        AqlValue subTwo =
            value.get(*resolver, StaticStrings::RevString, mustDestroyToo, false);
        AqlValueGuard guard(subTwo, mustDestroyToo);
        if (subTwo.isString()) {
          rev.assign(subTwo.slice().copyString());
        }
      }

      return TRI_ERROR_NO_ERROR;
    }
  } else if (value.isString()) {
    key.assign(value.slice().copyString());
    return TRI_ERROR_NO_ERROR;
  }

  return TRI_ERROR_ARANGO_DOCUMENT_KEY_MISSING;
}

int extractKey(transaction::Methods* trx, AqlValue const& value, std::string& key) {
  std::string optimizeAway;
  return extractKeyAndRev(trx, value, key, optimizeAway, true /*key only*/);
}

/// @brief process the result of a data-modification operation
void handleStats(ModificationExecutorBase::Stats& stats,
                 ModificationExecutorInfos& info, int code, bool ignoreErrors,
                 std::string const* errorMessage = nullptr) {
  if (code == TRI_ERROR_NO_ERROR) {
    // update the success counter
    if (info._doCount) {
      stats.incrWritesExecuted();
    }
    return;
  }

  if (ignoreErrors) {
    // update the ignored counter
    if (info._doCount) {
      stats.incrWritesExecuted();
    }
    return;
  }

  // bubble up the error
  if (errorMessage != nullptr && !errorMessage->empty()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(code, *errorMessage);
  }

  THROW_ARANGO_EXCEPTION(code);
}

/// @brief process the result of a data-modification operation
void handleBabyStats(ModificationExecutorBase::Stats& stats, ModificationExecutorInfos& info,
                     std::unordered_map<int, size_t> const& errorCounter, uint64_t numBabies,
                     bool ignoreErrors, bool ignoreDocumentNotFound = false) {
  size_t numberBabies = numBabies;  // from uint64_t to size_t

  if (errorCounter.empty()) {
    // update the success counter
    // All successful.
    if (info._doCount) {
      stats.addWritesExecuted(numberBabies);
    }
    return;
  }

  if (ignoreErrors) {
    for (auto const& pair : errorCounter) {
      // update the ignored counter
      if (info._doCount) {
        stats.addWritesIgnored(pair.second);
      }
      numberBabies -= pair.second;
    }

    // update the success counter
    if (info._doCount) {
      stats.addWritesExecuted(numberBabies);
    }
    return;
  }
  auto first = errorCounter.begin();
  if (ignoreDocumentNotFound && first->first == TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) {
    if (errorCounter.size() == 1) {
      // We only have Document not found. Fix statistics and ignore
      // update the ignored counter
      if (info._doCount) {
        stats.addWritesIgnored(first->second);
      }
      numberBabies -= first->second;
      // update the success counter
      if (info._doCount) {
        stats.addWritesExecuted(numberBabies);
      }
      return;
    }

    // Sorry we have other errors as well.
    // No point in fixing statistics.
    // Throw other error.
    ++first;
    TRI_ASSERT(first != errorCounter.end());
  }

  THROW_ARANGO_EXCEPTION(first->first);
}
}  // namespace

///////////////////////////////////////////////////////////////////////////////
// INSERT /////////////////////////////////////////////////////////////////////
template<bool pass>
bool Insert::doModifications(ModificationExecutorInfos& info, SingleBlockFetcher<pass>& fetcher,
                             ModificationExecutorBase::Stats& stats) {
  OperationOptions& options = info._options;

  reset();
  _tmpBuilder.openArray();

  const RegisterId& inReg = info._input1RegisterId.value();

  fetcher.forRowInBlock([this, inReg, &info](InputAqlItemRow&& row) {
    auto const& inVal = row.getValue(inReg);
    if (!info._consultAqlWriteFilter ||
        info._aqlCollection->getCollection()->skipForAqlWrite(inVal.slice(),
                                                              StaticStrings::Empty)) {
      _operations.push_back(ModOperationType::APPLY_RETURN);
      // TODO This may be optimized with externals
      _tmpBuilder.add(inVal.slice());
    } else {
      // not relevant for ourselves... just pass it on to the next block
      _operations.push_back(ModOperationType::IGNORE_RETURN);
    }
  });

  TRI_ASSERT(_operations.size() == fetcher.currentBlock()->block().size());

  _tmpBuilder.close();
  auto toInsert = _tmpBuilder.slice();

  // At this point _tempbuilder contains the objects to insert
  // and _operations the information if the data is to be kept or not

  TRI_ASSERT(toInsert.isArray());
  try {
    LOG_DEVEL << "num to insert: " << toInsert.length();
  } catch (...) {
  }

  //TRI_ASSERT(toInsert.isArray());
  //try {
  //  LOG_DEVEL << "to insert: " << toInsert.toJson();
  //} catch (...) {
  //}

  // former - skip empty
  // no more to prepare
  if (toInsert.length() == 0) {
//    executor._copyBlock = true;
    LOG_DEVEL << "THIS IS BAD!";
    TRI_ASSERT(false);
    return true;
  }

  // execute insert
  TRI_ASSERT(info._trx);
  auto operationResult = info._trx->insert(info._aqlCollection->name(), toInsert, options);
  setOperationResult(std::move(operationResult));


  // handle statisitcs
  handleBabyStats(stats, info, _operationResult.countErrorCodes,
                  toInsert.length(), info._ignoreErrors);

  _tmpBuilder.clear();

  if (_operationResult.fail()) {
    THROW_ARANGO_EXCEPTION(_operationResult.result);
  }

  if (!options.silent) {
    TRI_ASSERT(_operationResult.buffer != nullptr);
    TRI_ASSERT(_operationResult.slice().isArray());

    // former - skip empty
    if (_operationResultArraySlice.length() == 0) {
//      executor._copyBlock = true;
      TRI_ASSERT(false);
      return true;
    }
  }
  return true;
}

bool Insert::doOutput(ModificationExecutorInfos& info, OutputAqlItemRow& output) {
  TRI_ASSERT(_block);
  TRI_ASSERT(_block->hasBlock());
  TRI_ASSERT(_blockIndex < _block->block().size());

  OperationOptions& options = info._options;

  InputAqlItemRow input = InputAqlItemRow(_block, _blockIndex);
  if (!options.silent) {
    if (_operations[_blockIndex] == ModOperationType::APPLY_RETURN) {
      TRI_ASSERT(_operationResultIterator.valid());
      auto elm = _operationResultIterator.value();

      bool wasError =
          arangodb::basics::VelocyPackHelper::getBooleanValue(elm, StaticStrings::Error, false);

      if (!wasError) {
        if (options.returnNew) {
          AqlValue value(elm.get("new"));
          AqlValueGuard guard(value, true);
          // store $NEW
          output.moveValueInto(info._outputNewRegisterId.value(), input, guard);
        }
        if (options.returnOld) {
          // store $OLD
          auto slice = elm.get("old");
          if (slice.isNone()) {
            AqlValue value(VPackSlice::nullSlice());
            AqlValueGuard guard(value, true);
            output.moveValueInto(info._outputOldRegisterId.value(), input, guard);
          } else {
            AqlValue value(slice);
            AqlValueGuard guard(value, true);
            output.moveValueInto(info._outputOldRegisterId.value(), input, guard);
          }
        }
      }  // !wasError - end
      ++_operationResultIterator;
    } else if (_operations[_blockIndex] == ModOperationType::IGNORE_RETURN) {
      output.copyRow(input);
    } else {
      TRI_ASSERT(false);
    }

  } else {
    output.copyRow(input);
  }

  // increase index and make sure next element is within the valid range
  return ++_blockIndex < _block->block().size();
}

///////////////////////////////////////////////////////////////////////////////
// REMOVE /////////////////////////////////////////////////////////////////////
template<bool pass>
bool Remove::doModifications(ModificationExecutorInfos& info, SingleBlockFetcher<pass>& fetcher,
                             ModificationExecutorBase::Stats& stats) {
  OperationOptions& options = info._options;

  reset();
  _tmpBuilder.openArray();

  auto* trx = info._trx;
  int errorCode = TRI_ERROR_NO_ERROR;
  std::string key;
  std::string rev;

  const RegisterId& inReg = info._input1RegisterId.value();
  fetcher.forRowInBlock([this, &stats, &errorCode, &key, &rev, trx,
                                   inReg, &info](InputAqlItemRow&& row) {
    auto const& inVal = row.getValue(inReg);
    if (!info._consultAqlWriteFilter ||
        info._aqlCollection->getCollection()->skipForAqlWrite(inVal.slice(),
                                                              StaticStrings::Empty)) {
      key.clear();
      rev.clear();

      if (inVal.isObject()) {
        errorCode = extractKeyAndRev(trx, inVal, key, rev, info._options.ignoreRevs /*key only*/);
      } else if (inVal.isString()) {
        // value is a string
        key = inVal.slice().copyString();
      } else {
        errorCode = TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
      }

      if (errorCode == TRI_ERROR_NO_ERROR) {
        _operations.push_back(ModOperationType::APPLY_RETURN);

        // no error. we expect to have a key
        // create a slice for the key
        _tmpBuilder.openObject();
        _tmpBuilder.add(StaticStrings::KeyString, VPackValue(key));
        if (!info._options.ignoreRevs && !rev.empty()) {
          _tmpBuilder.add(StaticStrings::RevString, VPackValue(rev));
        }
        _tmpBuilder.close();
      } else {
        // We have an error, handle it
        _operations.push_back(ModOperationType::IGNORE_SKIP);
        handleStats(stats, info, errorCode, info._ignoreErrors);
      }
    } else {
      // not relevant for ourselves... just pass it on to the next block
      _operations.push_back(ModOperationType::IGNORE_RETURN);
    }
  });

  TRI_ASSERT(_operations.size() == fetcher.currentBlock()->block().size());

  _tmpBuilder.close();
  auto toRemove = _tmpBuilder.slice();

  // At this point _tempbuilder contains the objects to insert
  // and _operations the information if the data is to be kept or not

  // TRI_ASSERT(toInsert.isArray());
  // try {
  //  LOG_DEVEL << "num to insert" << toInsert.length();
  //} catch (...) {
  //}

  // former - skip empty
  // no more to prepare
  if (toRemove.length() == 0) {
//    executor._copyBlock = true;
    TRI_ASSERT(false);
    return true;
  }

  // execute insert
  TRI_ASSERT(info._trx);
  auto operationResult = info._trx->remove(info._aqlCollection->name(), toRemove, options);
  setOperationResult(std::move(operationResult));


  // handle statisitcs
  handleBabyStats(stats, info, _operationResult.countErrorCodes,
                  toRemove.length(), info._ignoreErrors);

  _tmpBuilder.clear();

  if (_operationResult.fail()) {
    THROW_ARANGO_EXCEPTION(_operationResult.result);
  }

  if (!options.silent) {
    TRI_ASSERT(_operationResult.buffer != nullptr);
    TRI_ASSERT(_operationResult.slice().isArray());

    // former - skip empty
    if (_operationResultArraySlice.length() == 0) {
//      executor._copyBlock = true;
      TRI_ASSERT(false);
      return true;
    }
  }
  return true;
}

bool Remove::doOutput(ModificationExecutorInfos& info, OutputAqlItemRow& output) {
  TRI_ASSERT(_block);
  TRI_ASSERT(_block->hasBlock());
  TRI_ASSERT(_blockIndex < _block->block().size());

  OperationOptions& options = info._options;

  InputAqlItemRow input = InputAqlItemRow(_block, _blockIndex);
  if (!options.silent) {
    if (_operations[_blockIndex] == ModOperationType::APPLY_RETURN) {
      TRI_ASSERT(_operationResultIterator.valid());
      auto elm = _operationResultIterator.value();

      bool wasError =
          arangodb::basics::VelocyPackHelper::getBooleanValue(elm, StaticStrings::Error, false);

      if (!wasError) {
        if (options.returnOld) {
          // store $OLD
          auto slice = elm.get("old");

          // original no none check! //result->emplaceValue(dstRow, _outRegOld, elm.get("old"));
          // if (slice.isNone()) {
          //   AqlValue value(VPackSlice::nullSlice());
          //   AqlValueGuard guard(value, true);
          //   output.moveValueInto(info._outputOldRegisterId.value(), input, guard);
          // } else {
          AqlValue value(slice);
          AqlValueGuard guard(value, true);
          output.moveValueInto(info._outputOldRegisterId.value(), input, guard);
          //}
        }
      }
      ++_operationResultIterator;
    } else if (_operations[_blockIndex] == ModOperationType::IGNORE_RETURN) {
      output.copyRow(input);
    } else {
      TRI_ASSERT(false);
    }

  } else {
    output.copyRow(input);
  }

  // increase index and make sure next element is within the valid range
  return ++_blockIndex < _block->block().size();
}

///////////////////////////////////////////////////////////////////////////////
// UPSERT /////////////////////////////////////////////////////////////////////
template<bool pass>
bool Upsert::doModifications(ModificationExecutorInfos& info, SingleBlockFetcher<pass>& fetcher,
                             ModificationExecutorBase::Stats& stats) {
  OperationOptions& options = info._options;

  reset();

  _insertBuilder.openArray();
  _updateBuilder.openArray();

  int errorCode = TRI_ERROR_NO_ERROR;
  std::string errorMessage;
  std::string key;
  auto* trx = info._trx;
  const RegisterId& inDocReg = info._input1RegisterId.value();
  const RegisterId& insertReg = info._input2RegisterId.value();
  const RegisterId& updateReg = info._input3RegisterId.value();

  fetcher.forRowInBlock([this, &stats, &errorCode, &errorMessage,
                                   &key, trx, inDocReg, insertReg, updateReg,
                                   &info](InputAqlItemRow&& row) {
    auto const& inVal = row.getValue(inDocReg);
    if (inVal.isObject()) /*update case, as old doc is present*/ {
      if (!info._consultAqlWriteFilter ||
          info._aqlCollection->getCollection()->skipForAqlWrite(inVal.slice(),
                                                                StaticStrings::Empty)) {
        key.clear();
        errorCode = extractKey(trx, inVal, key);
        if (errorCode == TRI_ERROR_NO_ERROR) {
          auto const& updateDoc = row.getValue(updateReg);
          if (updateDoc.isObject()) {
            VPackSlice toUpdate = updateDoc.slice();

            _tmpBuilder.clear();
            _tmpBuilder.openObject();
            _tmpBuilder.add(StaticStrings::KeyString, VPackValue(key));
            _tmpBuilder.close();

            VPackBuilder tmp =
                VPackCollection::merge(toUpdate, _tmpBuilder.slice(), false, false);
            _updateBuilder.add(tmp.slice());
            _operations.push_back(ModOperationType::APPLY_UPDATE);
          } else {
            errorCode = TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
            errorMessage = std::string("expecting 'Object', got: ") +
                           updateDoc.slice().typeName() +
                           std::string(" while handling: UPSERT");
          }
        }
      } else /*Doc is not relevant ourselves. Just pass Row to the next block*/ {
        _operations.push_back(ModOperationType::IGNORE_RETURN);
      }
    } else /*insert case*/ {
      auto const& toInsert = row.getValue(insertReg).slice();
      if (toInsert.isObject()) {
        if (!info._consultAqlWriteFilter ||
            !info._aqlCollection->getCollection()->skipForAqlWrite(toInsert, StaticStrings::Empty)) {
          _insertBuilder.add(toInsert);
          _operations.push_back(ModOperationType::APPLY_INSERT);
        } else {
          // not relevant for ourselves... just pass it on to the next block
          _operations.push_back(ModOperationType::IGNORE_RETURN);
        }
      } else {
        errorCode = TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
        errorMessage = std::string("expecting 'Object', got: ") + toInsert.typeName() +
                       std::string(" while handling: UPSERT");
      }

      if (errorCode != TRI_ERROR_NO_ERROR) {
        _operations.push_back(ModOperationType::IGNORE_SKIP);
        handleStats(stats, info, errorCode, info._ignoreErrors, &errorMessage);
      }
    }
  });

  TRI_ASSERT(_operations.size() == fetcher.currentBlock()->block().size());

  _insertBuilder.close();
  _updateBuilder.close();

  auto toInsert = _insertBuilder.slice();
  auto toUpdate = _updateBuilder.slice();

  // former - skip empty
  // no more to prepare
  if (toInsert.length() == 0 && toUpdate.length() == 0) {
//    executor._copyBlock = true;
    TRI_ASSERT(false);
    return true;
  }

  // execute insert
  TRI_ASSERT(info._trx);
  // we use _operationResult as insertResult
  //
  OperationResult opRes;  // temporaroy value
  if (toInsert.isArray() && toInsert.length() > 0) {
    OperationResult opRes =
        info._trx->insert(info._aqlCollection->name(), toInsert, options);
    setOperationResult(std::move(opRes));

    if (_operationResult.fail()) {
      THROW_ARANGO_EXCEPTION(_operationResult.result);
    }

    handleBabyStats(stats, info, _operationResult.countErrorCodes,
                    toInsert.length(), info._ignoreErrors);

    _insertBuilder.clear();
  }

  if (toUpdate.isArray() && toUpdate.length() > 0) {
    if (info._isReplace) {
      opRes = info._trx->replace(info._aqlCollection->name(), toUpdate, options);
    } else {
      opRes = info._trx->update(info._aqlCollection->name(), toUpdate, options);
    }
    setOperationResultUpdate(std::move(opRes));

    if (_operationResultUpdate.fail()) {
      THROW_ARANGO_EXCEPTION(_operationResultUpdate.result);
    }

    handleBabyStats(stats, info, _operationResultUpdate.countErrorCodes,
                    toUpdate.length(), info._ignoreErrors);

    _tmpBuilder.clear();
    _updateBuilder.clear();

  }
  // former - skip empty
  if (_operationResultArraySlice.length() == 0) {
//    executor._copyBlock = true;
    TRI_ASSERT(false);
    return true;
  }
  return true;
}

bool Upsert::doOutput(ModificationExecutorInfos& info, OutputAqlItemRow& output) {
  TRI_ASSERT(_block);
  TRI_ASSERT(_block->hasBlock());
  TRI_ASSERT(_blockIndex < _block->block().size());

  OperationOptions& options = info._options;

  InputAqlItemRow input = InputAqlItemRow(_block, _blockIndex);
  if (!options.silent) {
    auto& op = _operations[_blockIndex];
    if (op == ModOperationType::APPLY_UPDATE || op == ModOperationType::APPLY_INSERT) {
      TRI_ASSERT(_operationResultIterator.valid());        // insert
      TRI_ASSERT(_operationResultUpdateIterator.valid());  // update

      // fetch operation type (insert or update/replace)
      VPackArrayIterator* iter = &_operationResultIterator;
      if (_operations[_blockIndex] == ModOperationType::APPLY_UPDATE) {
        iter = &_operationResultUpdateIterator;
      }
      auto elm = iter->value();

      bool wasError =
          arangodb::basics::VelocyPackHelper::getBooleanValue(elm, StaticStrings::Error, false);

      if (!wasError) {
        if (options.returnNew) {
          AqlValue value(elm.get("new"));
          AqlValueGuard guard(value, true);
          // store $NEW
          output.moveValueInto(info._outputNewRegisterId.value(), input, guard);
        }
      }
      ++*iter;
    } else if (_operations[_blockIndex] == ModOperationType::IGNORE_SKIP) {
      output.copyRow(input);
    } else {
      TRI_ASSERT(false);
    }

  } else {
    output.copyRow(input);
  }

  // increase index and make sure next element is within the valid range
  return ++_blockIndex < _block->block().size();
}

///////////////////////////////////////////////////////////////////////////////
// UPDATEREPLACE //////////////////////////////////////////////////////////////
template <typename ModType>
template<bool pass>
bool UpdateReplace<ModType>::doModifications(ModificationExecutorInfos& info, SingleBlockFetcher<pass>& fetcher,
                                             ModificationExecutorBase::Stats& stats) {
  OperationOptions& options = info._options;

  //  if (!producesOutput && isDBServer && ignoreDocumentNotFound) {
  //    // on a DB server, when we are told to ignore missing documents, we must
  //    // set this flag in order to not assert later on
  //    producesOutput = true;
  //  }

  // check if we're a DB server in a cluster
  bool const isDBServer = ServerState::instance()->isDBServer();
  info._producesResults =
      info._producesResults || (isDBServer && info._ignoreDocumentNotFound);

  reset();
  _updateOrReplaceBuilder.openArray();

  int errorCode = TRI_ERROR_NO_ERROR;
  std::string errorMessage;
  std::string key;
  std::string rev;
  auto* trx = info._trx;
  const RegisterId& inDocReg = info._input1RegisterId.value();
  const RegisterId& keyReg = info._input2RegisterId.get();  // could be uninuitalized
  const bool hasKeyVariable = info._input2RegisterId.has_value();

  // const RegisterId& updateReg = info._input3RegisterId.value();

  fetcher.forRowInBlock([this, &options, &stats, &errorCode,
                                   &errorMessage, &key, &rev, trx, inDocReg, keyReg,
                                   hasKeyVariable, &info](InputAqlItemRow&& row) {
    auto const& inVal = row.getValue(inDocReg);
    errorCode = TRI_ERROR_NO_ERROR;
    errorMessage.clear();

    key.clear();
    rev.clear();

    auto const& inDoc = row.getValue(inDocReg);
    if (inDoc.isObject()) {
      if (hasKeyVariable) {
        AqlValue const& keyVal = row.getValue(keyReg);
        if (options.ignoreRevs) {
          errorCode = extractKey(trx, keyVal, key);
        } else {
          errorCode = extractKeyAndRev(trx, keyVal, key, rev);
        }
      } else /*!hasKeyVariable*/ {
        errorCode = extractKey(trx, inVal, key);
      }
    } else /*inDoc is not an object*/ {
      errorCode = TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID;
      errorMessage = std::string("expecting 'Object', got: ") + inVal.slice().typeName() +
                     std::string(" while handling: ") + _name;
    }

    if (errorCode == TRI_ERROR_NO_ERROR) {
      if (!info._consultAqlWriteFilter ||
          !info._aqlCollection->getCollection()->skipForAqlWrite(inVal.slice(), key)) {
        _operations.push_back(ModOperationType::APPLY_RETURN);
        if (hasKeyVariable) {
          _tmpBuilder.clear();
          _tmpBuilder.openObject();
          _tmpBuilder.add(StaticStrings::KeyString, VPackValue(key));
          if (!options.ignoreRevs && !rev.empty()) {
            _tmpBuilder.add(StaticStrings::RevString, VPackValue(rev));
          } else {
            // we must never take _rev from the document if there is a key
            // expression.
            _tmpBuilder.add(StaticStrings::RevString, VPackValue(VPackValueType::Null));
          }
          _tmpBuilder.close();
          VPackCollection::merge(_updateOrReplaceBuilder, inVal.slice(),
                                 _tmpBuilder.slice(), false, true);
        } else {
          // use original slice for updating
          _updateOrReplaceBuilder.add(inVal.slice());
        }
      } else {
        // not relevant for ourselves... just pass it on to the next block
        _operations.push_back(ModOperationType::IGNORE_RETURN);
      }
    } else {
      _operations.push_back(ModOperationType::IGNORE_SKIP);
      handleStats(stats, info, errorCode, info._ignoreErrors, &errorMessage);
    }
  });

  TRI_ASSERT(_operations.size() == fetcher.currentBlock()->block().size());

  _updateOrReplaceBuilder.close();

  auto toUpdateOrReplace = _updateOrReplaceBuilder.slice();

  // former - skip empty
  // no more to prepare
  if (toUpdateOrReplace.length() == 0 && toUpdateOrReplace.length() == 0) {
//    executor._copyBlock = true;
    TRI_ASSERT(false);
    return true;
  }

  TRI_ASSERT(info._trx);

  if (toUpdateOrReplace.isArray() && toUpdateOrReplace.length() > 0) {
    OperationResult opRes =
        (info._trx->*_method)(info._aqlCollection->name(), toUpdateOrReplace, options);
    setOperationResult(std::move(opRes));

    if (_operationResult.fail()) {
      THROW_ARANGO_EXCEPTION(_operationResult.result);
    }

    handleBabyStats(stats, info, _operationResult.countErrorCodes,
                    toUpdateOrReplace.length(), info._ignoreErrors);
  }

  _tmpBuilder.clear();
  _updateOrReplaceBuilder.clear();

  // former - skip empty
  if (_operationResultArraySlice.length() == 0) {
//    executor._copyBlock = true;
    TRI_ASSERT(false);
    return true;
  }
  return true;
}

template <typename ModType>
bool UpdateReplace<ModType>::doOutput(ModificationExecutorInfos &info,
                                      OutputAqlItemRow& output) {
  TRI_ASSERT(_block);
  TRI_ASSERT(_block->hasBlock());
  TRI_ASSERT(_blockIndex < _block->block().size());
  TRI_ASSERT(_operationResultArraySlice.isArray());

  OperationOptions& options = info._options;

  InputAqlItemRow input = InputAqlItemRow(_block, _blockIndex);
  if (_operations[_blockIndex] == ModOperationType::APPLY_RETURN) {
    TRI_ASSERT(_operationResultIterator.valid());
    auto elm = _operationResultIterator.value();

    bool wasError =
        arangodb::basics::VelocyPackHelper::getBooleanValue(elm, StaticStrings::Error, false);

    if (!wasError) {
      if (options.returnNew) {
        AqlValue value(elm.get("new"));
        AqlValueGuard guard(value, true);
        // store $NEW
        output.moveValueInto(info._outputNewRegisterId.value(), input, guard);
      }
      if (options.returnOld) {
        auto slice = elm.get("old");
        AqlValue value(slice);
        AqlValueGuard guard(value, true);
        // store $OLD
        output.moveValueInto(info._outputOldRegisterId.value(), input, guard);
      }
    }
    ++_operationResultIterator;
  } else if (_operations[_blockIndex] == ModOperationType::IGNORE_SKIP) {
    output.copyRow(input);
  } else {
    TRI_ASSERT(false);
  }

  // increase index and make sure next element is within the valid range
  return ++_blockIndex < _block->block().size();
}

template struct arangodb::aql::UpdateReplace<Update>;
template struct arangodb::aql::UpdateReplace<Replace>;

template bool arangodb::aql::Insert::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<true>&, ModificationExecutorBase::Stats&);
template bool arangodb::aql::Insert::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<false>&, ModificationExecutorBase::Stats&);

template bool arangodb::aql::Remove::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<true>&, ModificationExecutorBase::Stats&);
template bool arangodb::aql::Remove::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<false>&, ModificationExecutorBase::Stats&);

template bool arangodb::aql::Upsert::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<true>&, ModificationExecutorBase::Stats&);
template bool arangodb::aql::Upsert::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<false>&, ModificationExecutorBase::Stats&);

//template bool arangodb::aql::Update::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<true>&, ModificationExecutorBase::Stats&);
//template bool arangodb::aql::Update::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<false>&, ModificationExecutorBase::Stats&);

template bool arangodb::aql::UpdateReplace<Update>::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<true>&, ModificationExecutorBase::Stats&);
template bool arangodb::aql::UpdateReplace<Update>::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<false>&, ModificationExecutorBase::Stats&);

template bool arangodb::aql::UpdateReplace<Replace>::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<true>&, ModificationExecutorBase::Stats&);
template bool arangodb::aql::UpdateReplace<Replace>::doModifications(ModificationExecutorInfos&, SingleBlockFetcher<false>&, ModificationExecutorBase::Stats&);
