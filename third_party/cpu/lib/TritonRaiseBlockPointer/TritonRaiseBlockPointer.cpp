//===----------------------------------------------------------------------===//
//
// Copyright (c) Microsoft Corporation, Meta Platforms.
// Licensed under the MIT license.
//
//===----------------------------------------------------------------------===//

#include "cpu/include/TritonRaiseBlockPointer/Passes.h"

#include "mlir/IR/Matchers.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"
#include <set>

#define DEBUG_TYPE "triton-cpu-raise-block-pointer"

// This pass does manage to raise tensor of pointers into block pointers for
// simple cases (e.g. 03 matmul tutorial). However, this pass has several know
// limitations:
//   - Masks and modulos are not correctly handled by this pass. Issue #1784
//   (https://github.com/intel/intel-xpu-backend-for-triton/issues/1784) has
//   been created to address this limitation.
//   - The pattern matching method used in this pass makes it prone to fail
//   raising memory accesses. For the moment, the most fragile part of the pass
//   is probably the support for fixing the axis of the offsets
//   (see comment l.867).

using namespace mlir;

namespace mlir::triton::cpu {
#define GEN_PASS_DEF_TRITONRAISEBLOCKPOINTER
#include "cpu/include/TritonRaiseBlockPointer/Passes.h.inc"
} // namespace mlir::triton::cpu

namespace {
constexpr unsigned offsetBitwidth = 32;
constexpr unsigned shapeAndStridesBitwidth = 64;

// FROM intel-xpu-backend-for-triton/third_party/intel/lib/TritonIntelGPUTransforms/Utility.cpp
static std::optional<int64_t> getIntAttr(const OpFoldResult ofr) {
  if (ofr.is<Attribute>() && isa<IntegerAttr>(ofr.get<Attribute>()))
    return cast<IntegerAttr>(ofr.get<Attribute>()).getInt();
  return std::nullopt;
}

// FROM intel-xpu-backend-for-triton/third_party/intel/lib/TritonIntelGPUTransforms/Utility.cpp
std::optional<int64_t> getFoldedConstantValue(Operation *op) {
  SmallVector<OpFoldResult> results;
  if (failed(op->fold(results))) {
    return std::nullopt;
  }

  // If fold succeeded but `results` is empty, we give a second try, after the
  // operands have been switched during the first call to `fold()`.
  if (results.empty()) {
    if (failed(op->fold(results))) {
      return std::nullopt;
    }
  }

  if (results.size() != 1) {
    return std::nullopt;
  }

  auto intAttr = getIntAttr(results[0]);
  if (intAttr.has_value()) {
    return intAttr.value();
  }

  auto val = cast<Value>(results[0]);
  auto constOp = val.getDefiningOp<arith::ConstantOp>();
  if (!constOp)
    return std::nullopt;

  return getIntAttr(constOp.getValue());
}

// FROM intel-xpu-backend-for-triton/third_party/intel/lib/TritonIntelGPUTransforms/Utility.cpp
bool isConstant(Value val, const unsigned expected) {
  auto defOp = val.getDefiningOp();
  if (!defOp)
    return false;
  return (getFoldedConstantValue(defOp) == expected);
}

// Data structure used to decode pointer arithmetics. Offsets, sizes, and
// strides are in unit of elements in a linearly laid-out memory, which is the
// same as pointer arithmetic operations in Triton language. Scalar is a
// shortcut used when the entire state describes a single scalar value. Source
// is the base pointer. If order is present, PtrState describes block pointer;
// otherwise it describes non-block pointers. When it describes block pointer,
// shape field means the same field as tt.make_tensor_ptr; when it describes a
// non-block pointer, shape field indicates how address wraps around (i.e.,
// modulo); a constant 0 indicates no modulo for the dimension.
struct PtrState {

  SmallVector<Value> offsets;
  SmallVector<Value> strides;
  SmallVector<Value> shape;
  SmallVector<int32_t> sizes;
  SmallVector<int32_t> order;

  Value source;
  Value scalar;

  int32_t getRank() const {
    assert(offsets.size() == sizes.size() && offsets.size() == strides.size() &&
           offsets.size() == strides.size());
    return offsets.size();
  }

  // @return true if the `PtrState` structure describes a block pointer,
  // otherwise it describes a non-block pointer.
  bool isBlockPtr() const { return !order.empty(); }

  // This function checks whether the pointer addresses wraps around on the
  // dimention `dim`.
  // @return true if the address wraps around, (i.e. has modulo).
  // Note that this function should only be called when PtrState describes a
  // non-block pointer.
  bool dimHasModulo(uint32_t dim) const {
    assert(
        !isBlockPtr() &&
        "Analysis should not check modulo if PtrState describes block pointer");

    assert(dim < getRank() && "Dim cannot be higher than the tensor rank.");

    // When PtrState describes a non-block pointer, shape field indicates how
    // address wraps around. As a result, a constant 0 indicates no wrap around
    // (i.e. modulo) for the dimension.
    return !isConstant(shape[dim], 0);
  }

  // @return true if addresses wrap around in any of the pointer dimension.
  bool hasModulo() const {
    for (int32_t i = 0; i < getRank(); i++) {
      if (dimHasModulo(i)) {
        return true;
      }
    }
    return false;
  }

  bool isEmpty() const { return getRank() == 0 && !source && !scalar; }

  // Process addition of two PtrStates.
  LogicalResult addState(const PtrState &lhsState, const PtrState &rhsState,
                         Operation *op, OpBuilder &builder) {
    assert(isEmpty() && lhsState.getRank() == rhsState.getRank());
    Location loc = op->getLoc();

    if (lhsState.source && rhsState.source) {
      op->emitRemark("TritonRaiseBlockPointer: do not support adding two "
                     "pointer states that both have base pointers");
      return failure();
    }

    source = lhsState.source ? lhsState.source : rhsState.source;

    if (lhsState.scalar && rhsState.scalar) { // both lhs and rhs are scalars
      auto addOp =
          builder.create<arith::AddIOp>(loc, lhsState.scalar, rhsState.scalar);
      scalar = addOp.getResult();
    } else if (lhsState.getRank() == 0) {
      scalar = lhsState.scalar ? lhsState.scalar : rhsState.scalar;
    }

    ArithBuilder abuilder(builder, loc);
    for (uint64_t i = 0; i < lhsState.getRank(); ++i) {
      Value newOffset = abuilder.add(lhsState.offsets[i], rhsState.offsets[i]);
      offsets.push_back(newOffset);

      Value newStride = abuilder.add(lhsState.strides[i], rhsState.strides[i]);
      strides.push_back(newStride);

      sizes.push_back(lhsState.sizes[i]);
    }

    // AddPtr where both lhs and rhs containing modulo operators not supported
    if (lhsState.hasModulo() && rhsState.hasModulo()) {
      op->emitRemark(
          "TritonRaiseBlockPointer: do not support adding two pointer states "
          "that both have modulo");
      return failure();
    }

    assert(
        !(lhsState.hasModulo() || rhsState.hasModulo()) ||
        (lhsState.getRank() <= 2) &&
            "cannot have rank > 2 if operand one of the operands has a modulo");

    // dealing with modulo:
    // - If lhs has no modulo, skip
    // - If rhs has zero offset on dim i, we can just use lhs's modulo
    // - Else, the analysis fails

    // An example for the 3rd condition above can look like:
    // %0 = tt.splat %scalar
    // %1 = tt.splat %ptr
    // %2 = tt.arange
    // %3 = arith.remsi %2, %size
    // %4 = tt.addptr %1, %3
    // %5 = tt.addptr %4, %0
    // %5 may also occur in a loop to increment %4 every iteration.

    const PtrState *lhs = &lhsState;
    const PtrState *rhs = &rhsState;

    if (rhs->hasModulo()) {
      std::swap(lhs, rhs);
    }

    for (uint64_t i = 0; i < lhs->getRank(); i++) {
      if (!lhs->dimHasModulo(i)) {
        shape.push_back(lhs->shape[i]);
      } else if (isConstant(rhs->offsets[i], 0)) {
        shape.push_back(lhs->shape[i]);
      } else {
        op->emitRemark("TritonRaiseBlockPointer: do not support adding to "
                       "operand with modulo");
        return failure();
      }
    }

    return success();
  }

  LogicalResult mulState(const PtrState &lhsState, const PtrState &rhsState,
                         Operation *op, OpBuilder &builder) {
    assert(isEmpty() && lhsState.getRank() == rhsState.getRank());

    Location loc = op->getLoc();

    assert(!lhsState.source && !rhsState.source &&
           "Multiplying base pointer does not make sense");

    assert(!(lhsState.scalar && rhsState.scalar) &&
           "do not expect to see both lhs and rhs are scalars");

    // currently do not support both tensors are effectively non-scalar
    if (!lhsState.scalar && !rhsState.scalar) {
      op->emitRemark("TritonRaiseBlockPointer: only support multiplying "
                     "pointer states when one of them represent a scalar");
      return failure();
    }

    PtrState const *lhs = &lhsState;
    PtrState const *rhs = &rhsState;

    if (!rhs->scalar && lhs->scalar)
      std::swap(lhs, rhs);

    Value i32Scalar = getValueOrCreateCastToIndexLike(
        builder, loc, builder.getI32Type(), rhs->scalar);
    Value i64Scalar = getValueOrCreateCastToIndexLike(
        builder, loc, builder.getI64Type(), rhs->scalar);
    ArithBuilder abuilder(builder, loc);
    for (const auto &[offset, stride, dim, size] :
         llvm::zip(lhs->offsets, lhs->strides, lhs->shape, lhs->sizes)) {

      Value newOffset =
          abuilder.mul(getValueOrCreateCastToIndexLike(
                           builder, loc, builder.getI32Type(), offset),
                       i32Scalar);
      Value newStride =
          abuilder.mul(getValueOrCreateCastToIndexLike(
                           builder, loc, builder.getI64Type(), stride),
                       i64Scalar);
      Value newDim = abuilder.mul(getValueOrCreateCastToIndexLike(
                                      builder, loc, builder.getI64Type(), dim),
                                  i64Scalar);

      offsets.push_back(newOffset);
      strides.push_back(newStride);
      shape.push_back(newDim);
      sizes.push_back(size);
    }

    return success();
  }

  triton::MakeTensorPtrOp createTTMakeTensorPtrOp(OpBuilder &builder,
                                                  Location loc) {

    SmallVector<Value> newOffsets;
    SmallVector<Value> newStrides;
    SmallVector<Value> newShape;
    ArithBuilder abuilder(builder, loc);
    for (const auto &[offset, stride, dim] :
         llvm::zip(offsets, strides, shape)) {

      if (isConstant(stride, 0)) {
        newOffsets.push_back(getValueOrCreateCastToIndexLike(
            builder, loc, builder.getI32Type(), offset));
      } else {
        auto divOffset = builder.create<arith::DivUIOp>(
            loc, builder.getI32Type(),
            getValueOrCreateCastToIndexLike(builder, loc, builder.getI32Type(),
                                            offset),
            getValueOrCreateCastToIndexLike(builder, loc, builder.getI32Type(),
                                            stride));
        newOffsets.push_back(divOffset);
      }
      newStrides.push_back(getValueOrCreateCastToIndexLike(
          builder, loc, builder.getI64Type(), stride));
      newShape.push_back(getValueOrCreateCastToIndexLike(
          builder, loc, builder.getI64Type(), dim));
    }

    auto op = builder.create<triton::MakeTensorPtrOp>(
        loc, source, newShape, newStrides, newOffsets, sizes, order);
    LLVM_DEBUG(llvm::dbgs() << "creating tt.make_tensor_ptr:\n" << op << "\n";);
    return op;
  }
};

#ifndef NDEBUG
template <typename T>
static llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                     const SmallVector<T> &v) {
  os << "{";
  if (!v.empty()) {
    os << v.front();
    llvm::for_each(ArrayRef<T>(v).drop_front(),
                   [&os](const T &el) { os << ", " << el; });
  }
  return os << "}";
}

static llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                                     const PtrState &state) {
  return os << "<offsets=" << state.offsets << "> <sizes=" << state.sizes
            << "> <strides=" << state.strides << "> <shape=" << state.shape
            << "> <order=" << state.order << ">";
}
#endif

struct TritonRaiseBlockPointer
    : triton::cpu::impl::TritonRaiseBlockPointerBase<
          TritonRaiseBlockPointer> {
  using Base::Base;
  using IndexMapSet = std::map<int, std::set<int>>;
  SmallVector<Operation *> cleanUp;

  void runOnOperation() final {
    auto moduleOp = getOperation();

    if (failed(rewriteOp(moduleOp))) {
      moduleOp->emitWarning("TritonRaiseToBlockPointer failed");
    }

    for (auto op : cleanUp) {
      if (op->getUsers().empty())
        op->erase();
    }
  }

  LogicalResult rewriteOp(Operation *rootOp) {
    LLVM_DEBUG({
      llvm::dbgs() << "rewriting rootOp\n";
      rootOp->dump();
    });

    rootOp->walk<WalkOrder::PreOrder>([&](Operation *op) {
      if (op == rootOp) {
        return WalkResult::advance();
      }
      return TypeSwitch<Operation *, WalkResult>(op)
          .Case([this](triton::AddPtrOp addptr) {
            if (failed(rewriteAddPtrOp(addptr)))
              addptr->emitRemark(
                  "TritonRaiseToBlockPointer: Failed to rewrite");
            return WalkResult::advance();
          })
          .Case<triton::MakeTensorPtrOp>([&](auto maketptr) {
            if (failed(remapMakeTensorPtrOp(maketptr))) {
              maketptr->emitRemark("TritonRaiseToBlockPointer: Failed to "
                                   "rewrite MakeTensorPtrOp");
            }
            return WalkResult::advance();
          })
          .Case<triton::LoadOp, triton::StoreOp>([this](auto loadstore) {
            if (failed(rewriteLoadStoreOp(loadstore))) {
              loadstore->emitRemark(
                  "TritonRaiseToBlockPointer: Failed to rewrite");
              return WalkResult::advance();
            }
            return WalkResult::skip();
          })
          .Case<scf::ForOp>([&](auto forOp) {
            if (failed(rewriteForOp(forOp))) {
              forOp->emitRemark(
                  "TritonRaiseToBlockPointer: Failed to rewrite ForOp");
              return WalkResult::interrupt();
            }
            return WalkResult::skip();
          })
          .Default([&](auto) { return WalkResult::advance(); });
    });

    return success();
  }

  LogicalResult rewriteForOp(scf::ForOp op) {
    SmallVector<Value> newInitArgs;

    SmallVector<std::pair<int, PtrState>, 5> initArgIndexState;
    SmallVector<std::pair<int, PtrState>, 5> knownPtrsTmp;

    llvm::SmallDenseMap<int, PtrState> initArgIndexMap;

    OpBuilder builder(op);

    // Create a new list of init args
    for (auto [i, arg] : llvm::enumerate(op.getInitArgs())) {
      auto mappedV = ptrMap.lookupOrNull(arg);
      PtrState state;
      if (mappedV) {
        if (auto makeTensorPtrOp =
                mappedV.getDefiningOp<triton::MakeTensorPtrOp>()) {

          if (llvm::any_of(op.getRegionIterArgs()[i].getUsers(),
                           [](Operation *user) {
                             return isa<triton::ExpandDimsOp>(user);
                           })) {
            op->emitRemark("TritonRaiseToBlockPointer: ExpandDims Ops in loops "
                           "are currently not supported");
            return failure();
          }

          if (succeeded(visitOperandMakeTensorPtr(
                  makeTensorPtrOp, state, op.getLoc(), builder, true))) {
            newInitArgs.push_back(mappedV);
            // Record the PtrState for later processing
            initArgIndexState.push_back(std::make_pair(i, state));
            continue;
          }
        } else if (auto addptrOp = mappedV.getDefiningOp<triton::AddPtrOp>()) {
          // We always use tt.addptr for scalar pointers. If the defininig op is
          // tt.addptr and we have a non-scalar pointer, something must have
          // gone wrong with the pass.
          assert(!isa<RankedTensorType>(addptrOp.getResult().getType()) &&
                 "Result type of AddPtrOp must be a tensor!");
          if (succeeded(
                  visitOperandAddptr(addptrOp, state, op.getLoc(), builder))) {
            newInitArgs.push_back(mappedV);
            // Record the PtrState for later processing
            initArgIndexState.push_back(std::make_pair(i, state));
            continue;
          }
        }
      }
      // If any of the analysis failed, or init arg is not pointer related or
      // prior rewrite has failed. Pass as is
      newInitArgs.push_back(arg);
    }

    // For each of the PtrState recorded in the last step, insert new
    // instructions to describe offset and stride for each dimension and append
    // them to init args
    for (auto &[i, state] : initArgIndexState) {
      // For each dimension, if the corresponding offset and stride is an
      // integer attribute, create a constant value and append them at the
      // end of init arg list.
      for (auto [j, s] : llvm::enumerate(state.offsets)) {
        newInitArgs.push_back(s);
      }

      for (auto [j, s] : llvm::enumerate(state.strides)) {
        newInitArgs.push_back(s);
      }

      if (state.getRank() == 0) {
        assert(state.scalar &&
               "The state must have a scalar if its rank is equal to zero");
        // for scalar pointers, the scalar contains the offset and is the only
        // relevant state that could be updated by the loop.
        newInitArgs.push_back(state.scalar);
      }

      // Note that we want the knownPtrs to be indexed by block arg, but we
      // only have index for now. Also, the state we record is the init
      // arg, but want to use the newly created block arg. These block args
      // are not created yet. We will translate this mapping later.
      knownPtrsTmp.push_back(std::make_pair(i, state));
      levelToBlockArgIndex[level].insert(i);
    }

    // Create a new scf::ForOp that uses updated init args and same loop body
    auto newOp = builder.create<scf::ForOp>(
        op.getLoc(), op.getLowerBound(), op.getUpperBound(), op.getStep(),
        newInitArgs,
        [&](OpBuilder &b, Location loc, Value iv, ValueRange args) {
          IRMapping cloneMap;
          cloneMap.map(op.getInductionVar(), iv);
          cloneMap.map(op.getInitArgs(), newInitArgs);
          cloneMap.map(op.getRegionIterArgs(), args);

          for (auto &bodyOp : op.getRegion().getOps()) {
            b.clone(bodyOp, cloneMap);
          }
        });

    // Convert the book-keeping data structure to use the correct key and value.
    // Key is converted from init arg index to newly created block arg, and
    // Value's PtrState fields are converted from init arg to newly created
    // block arg
    int cnt = op.getRegionIterArgs().size();
    for (auto &[i, state] : knownPtrsTmp) {
      for (auto it = state.offsets.begin(); it != state.offsets.end(); it++) {
        *it = newOp.getRegionIterArgs()[cnt];
        cnt++;
      }

      for (auto it = state.strides.begin(); it != state.strides.end(); it++) {
        *it = newOp.getRegionIterArgs()[cnt];
        cnt++;
      }

      if (state.getRank() == 0) {
        assert(state.scalar &&
               "The state must have a scalar if its rank is equal to zero");
        state.scalar = newOp.getRegionIterArgs()[cnt];
        cnt++;
      }

      // Record the PtrState for this pointer
      auto key = newOp.getRegionIterArgs()[i];
      knownPtrs[key] = state;
      initArgIndexMap[i] = state;

      // For tensors of pointers, create a tt.make_block_ptr at the beginning of
      // the loop body that correspond to this region iter arg. In case it is
      // used by tt.load/tt.store in the loop body before pointer updates, this
      // will make sure rewriteLoadOp/rewriteStoreOp can use the analysis
      // result. E.g., given the following input (%tensor_of_ptr is a block
      // arg):
      // scf.for (%tensor_of_ptr) {
      //   %data = tt.load %tensor_of_ptr
      //   // more operations to update %tensor_of_ptr
      // }
      // We may produce the following output:
      // scf.for (%base_ptr, %stride, %offset) {
      //   %tensor_of_ptr = tt.make_block_ptr(%base_ptr, %stride, %offset)
      //   %data = tt.load %tensor_of_ptr
      //   // more operations to update %offset
      // }
      // If %tensor_of_ptr is not used (i.e., %tensor_of_ptr is updated before
      // used in the original IR), it will simply be removed by
      // canonicalization.

      // For scalar pointers, there is no need to create a tts.addptr at the
      // beginning of the loop body. We don't lower tt.load and tt.store on
      // scalars in this pass; pointer arithmetics can also just use the
      // original pointer.
      if (state.getRank() != 0) {
        OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(&newOp.getRegion().front());
        triton::MakeTensorPtrOp makePtrOp =
            state.createTTMakeTensorPtrOp(builder, op.getLoc());
        ptrMap.map(key, makePtrOp.getResult());
        knownPtrs[makePtrOp.getResult()] = std::move(state);
      }
    }

    for (auto &bodyOp : newOp.getRegion().getOps()) {
      if (auto forOp = dyn_cast<scf::ForOp>(bodyOp)) {
        forOp->emitRemark(
            "TritonRaiseToBlockPointer: nested loops currently not supported");
        return failure();
      }
    }
    // Update the loop body.
    if (failed(rewriteOp(newOp))) {
      newOp->erase();
      op->emitRemark("TritonRaiseToBlockPointer: update loop body failed when "
                     "rewriting for op");
      return failure();
    }
    if (op.getNumRegionIterArgs()) {
      auto yieldOp = cast<scf::YieldOp>(newOp.getBody()->getTerminator());
      if (failed(rewriteYieldOp(yieldOp, initArgIndexMap))) {
        newOp->erase();
        return failure();
      };
    }

    levelToBlockArgIndex.erase(level);

    // Replace only the results that correspond to the original scf.for
    auto resultsToReplaceWith = ResultRange(
        newOp.result_begin(), newOp.result_begin() + op.getNumResults());

    LLVM_DEBUG({
      llvm::dbgs() << "new for\n";
      newOp->print(llvm::dbgs(), OpPrintingFlags().printGenericOpForm());
      llvm::dbgs() << "\n";

      llvm::dbgs() << "old for\n";
      op->print(llvm::dbgs(), OpPrintingFlags().printGenericOpForm());
      llvm::dbgs() << "\n";
    });

    op->replaceAllUsesWith(resultsToReplaceWith);
    op->erase();

    return success();
  }

  LogicalResult
  rewriteYieldOp(scf::YieldOp op,
                 llvm::SmallDenseMap<int, PtrState> &knownPtrsFor) {
    if (levelToBlockArgIndex.find(level) == levelToBlockArgIndex.end()) {
      // no need to rewrite this op
      return success();
    }

    OpBuilder builder(op);

    // For each of the init arg that we added additional Values in for loop, we
    // need to add corresponding Values as yield operands. The loop below
    // gathers PtrState for those values.
    SmallVector<PtrState, 5> initArgState;
    for (auto [i, v] : llvm::enumerate(op->getOperands())) {
      // If this operand is not rewritten by forOp, skip
      auto &thisSet = levelToBlockArgIndex.find(level)->second;
      if (thisSet.find(i) == thisSet.end())
        continue;

      auto mappedV = ptrMap.lookupOrNull(v);
      if (!mappedV) {
        op->emitRemark("Prior rewrite failure lead to yield rewrite failure");
        return failure();
      }

      PtrState state;
      LogicalResult ret = failure();
      if (auto makeTPtrOp = mappedV.getDefiningOp<triton::MakeTensorPtrOp>()) {
        ret = visitOperandMakeTensorPtr(makeTPtrOp, state, op.getLoc(), builder,
                                        true);
      } else if (auto addptrOp = mappedV.getDefiningOp<triton::AddPtrOp>()) {
        ret = visitOperandAddptr(addptrOp, state, op.getLoc(), builder);
      }
      if (ret.failed()) {
        op->emitRemark("Failed to rewrite yield op");
        return failure();
      }
      initArgState.push_back(state);

      // Verify that shape is not updated during the for loop
      auto forState = knownPtrsFor[i];
      for (auto i = 0; i < forState.getRank(); ++i) {
        if (forState.shape[i] != state.shape[i]) {
          // Special case, see comments in addState in dealing with shape/modulo
          if (i == 0 && forState.getRank() == 2) {
            if (forState.shape[1] == state.shape[0] &&
                forState.shape[0] == state.shape[1]) {
              break;
            }
          }
          op->emitRemark(
              "TritonRaiseToBlockPointer: operand's shape/modulo state changed "
              "within loop body");
          return failure();
        }
      }
    }

    SmallVector<Value> operands;
    for (auto opnd : op->getOperands()) {
      auto mappedV = ptrMap.lookupOrNull(opnd);
      operands.push_back(mappedV ? mappedV : opnd);
    }

    // For each of the PtrState recorded in the last step, extract value
    // that correspond to offset and stride for each dimension and append
    // them to yield operands.
    for (auto state : initArgState) {
      for (auto s : state.offsets) {
        operands.push_back(s);
      }

      for (auto s : state.strides) {
        operands.push_back(s);
      }

      if (state.getRank() == 0) {
        operands.push_back(state.scalar);
      }
    }

    auto newOp = builder.create<scf::YieldOp>(op->getLoc(), operands);

    LLVM_DEBUG({
      llvm::dbgs() << "new yield:";
      newOp.getOperation()->print(llvm::dbgs(),
                                  OpPrintingFlags().printGenericOpForm());
      llvm::dbgs() << "\n";
    });

    op->erase();
    return success();
  }

  LogicalResult remapMakeTensorPtrOp(triton::MakeTensorPtrOp op) {
    OpBuilder builder(op);

    PtrState state;
    if (failed(visitOperandMakeTensorPtr(op, state, op.getLoc(), builder))) {
      return failure();
    }

    knownPtrs[op.getResult()] = std::move(state);
    return success();
  }

  Value getFinalValue(Value value) {
    auto defOp = value.getDefiningOp();
    if (!defOp) {
      // look init values outside the loop
      BlockArgument blockArg = dyn_cast<BlockArgument>(value);
      Operation *parentOp = blockArg.getOwner()->getParentOp();
      scf::ForOp forOp = dyn_cast<scf::ForOp>(parentOp);
      return forOp ? getFinalValue(
                         forOp.getInitArgs()[blockArg.getArgNumber() - 1])
                   : value;
    }

    if (isa<triton::ExpandDimsOp>(defOp) || isa<triton::BroadcastOp>(defOp) ||
        isa<triton::SplatOp>(defOp) || isa<arith::IndexCastOp>(defOp))
      return getFinalValue(defOp->getOperand(0));
    if (auto addOp = dyn_cast<arith::AddIOp>(defOp)) {
      if (isConstant(addOp.getLhs(), 0))
        return getFinalValue(addOp.getRhs());
      if (isConstant(addOp.getRhs(), 0))
        return getFinalValue(addOp.getLhs());
      return addOp.getResult();
    } else if (auto mulOp = dyn_cast<arith::MulIOp>(defOp)) {
      if (isConstant(mulOp.getLhs(), 1))
        return getFinalValue(mulOp.getRhs());
      if (isConstant(mulOp.getRhs(), 1))
        return getFinalValue(mulOp.getLhs());
      return mulOp.getResult();
    }
    return value;
  }

  bool lookForMulitplyingValueInDefiningPath(Value &val, Value &ref) {
    Operation *defOp = getFinalValue(val).getDefiningOp();
    if (!defOp)
      return false;

    if (auto mulOp = dyn_cast<arith::MulIOp>(defOp)) {
      if ((mulOp.getLhs() == ref) || (mulOp.getRhs() == ref))
        return true;
    }
    return false;
  }

  bool areValuesEqual(Value val1, Value val2) {
    if (val1 == val2)
      return true;
    Operation *op1 = val1.getDefiningOp();
    Operation *op2 = val2.getDefiningOp();
    if (op1 && op2) {
      auto intVal1 = getFoldedConstantValue(op1);
      auto intVal2 = getFoldedConstantValue(op2);
      if (intVal1.has_value() && intVal2.has_value()) {
        return intVal1.value() == intVal2.value();
      }
    }
    return false;
  }

  int checkIfOffsetMultipliedByStride(Value operand,
                                      SmallVector<Value> &strides) {
    Operation *defOp = operand.getDefiningOp();

    SmallVector<Value> finalStrides;
    // check all strides different
    // if not => skip
    for (auto stride : strides) {
      Value currentVal = getFinalValue(stride);
      if (llvm::any_of(finalStrides, [&](Value val) {
            return areValuesEqual(val, currentVal);
          }))
        return -1;
      finalStrides.push_back(currentVal);
    }

    int axis = 0;
    for (auto finalStride : finalStrides) {
      // search for a mul to finalStride in the predecessors
      if (lookForMulitplyingValueInDefiningPath(operand, finalStride))
        return axis;
      if (isConstant(finalStride, 1))
        return axis;
      ++axis;
    }
    return -1;
  }

  // Return true if a `triton::ExpandOp` has been found is the defining path.
  bool hasExpandOpInDefiningPath(Value value) {
    Operation *defOp = value.getDefiningOp();
    if (!defOp) {
      // look init values outside the loop
      BlockArgument blockArg = dyn_cast<BlockArgument>(value);
      Operation *parentOp = blockArg.getOwner()->getParentOp();
      scf::ForOp forOp = dyn_cast<scf::ForOp>(parentOp);
      return forOp ? hasExpandOpInDefiningPath(
                         forOp.getInitArgs()[blockArg.getArgNumber() - 1])
                   : false;
    }

    if (isa<triton::ExpandDimsOp>(defOp))
      return true;
    if (isa<arith::ConstantOp>(defOp))
      return false;
    if (isa<triton::MakeRangeOp>(defOp))
      return false;
    if (isa<triton::BroadcastOp>(defOp) || isa<triton::SplatOp>(defOp) ||
        isa<arith::IndexCastOp>(defOp) || isa<arith::RemUIOp>(defOp) ||
        isa<arith::RemSIOp>(defOp))
      return hasExpandOpInDefiningPath(defOp->getOperand(0));
    if (isa<arith::AddIOp>(defOp) || isa<arith::MulIOp>(defOp))
      return hasExpandOpInDefiningPath(defOp->getOperand(0)) ||
             hasExpandOpInDefiningPath(defOp->getOperand(1));

    return true;
  }

  LogicalResult rewriteAddPtrOp(triton::AddPtrOp op) {
    OpBuilder builder(op);
    Location loc = op.getLoc();

    PtrState state;
    if (failed(visitOperandAddptr(op, state, loc, builder)))
      return failure();

    knownPtrs[op.getResult()] = state;

    Value result = op.getResult();
    Value mapped = result;
    if (isa<RankedTensorType>(result.getType())) {
      triton::MakeTensorPtrOp makePtrOp =
          state.createTTMakeTensorPtrOp(builder, loc);
      knownPtrs[makePtrOp.getResult()] = std::move(state);
      mapped = makePtrOp.getResult();
    }

    ptrMap.map(result, mapped);

    // AddPtrOps that have been rewritten and no longer used in the code must be
    // removed in the pass to avoid type matching issue.
    cleanUp.push_back(op);

    return success();
  }

  LogicalResult visitOperandMakeTensorPtr(triton::MakeTensorPtrOp makeTPtrOp,
                                          PtrState &state, const Location loc,
                                          OpBuilder &builder,
                                          bool addedByPass = false) {
    assert(state.isEmpty() && "state is a return argument");

    if (auto iter = knownPtrs.find(makeTPtrOp.getResult());
        iter != knownPtrs.end()) {
      state = iter->second;
      return success();
    }

    state.source = makeTPtrOp.getBase();

    auto resType = cast<triton::PointerType>(makeTPtrOp.getResult().getType());
    auto pointeeType = cast<ShapedType>(resType.getPointeeType());
    auto shape = pointeeType.getShape();

    for (int64_t i = 0; i < pointeeType.getRank(); i++) {
      state.sizes.push_back(shape[i]);

      auto strideCst = builder.create<arith::IndexCastOp>(
          loc, builder.getIndexType(), makeTPtrOp.getStrides()[i]);
      auto offsetCst = builder.create<arith::IndexCastOp>(
          loc, builder.getIndexType(), makeTPtrOp.getOffsets()[i]);
      auto scaledOffset = builder.create<arith::MulIOp>(
          loc, offsetCst.getResult(), strideCst.getResult());
      state.offsets.push_back(getValueOrCreateCastToIndexLike(
          builder, loc, builder.getIntegerType(offsetBitwidth),
          scaledOffset.getResult()));
    }
    state.strides = makeTPtrOp.getStrides();
    state.shape = makeTPtrOp.getShape();
    state.order = SmallVector<int32_t>(makeTPtrOp.getOrder());

    return success();
  }

  LogicalResult visitOperandAddptr(triton::AddPtrOp addptrOp, PtrState &state,
                                   Location loc, OpBuilder &builder) {
    assert(state.isEmpty() && "state is a return argument");

    PtrState ptrState;
    if (failed(visitOperand(addptrOp.getPtr(), ptrState, addptrOp.getLoc(),
                            builder))) {
      return failure();
    }

    PtrState offsetState;
    if (failed(visitOperand(addptrOp.getOffset(), offsetState,
                            addptrOp.getLoc(), builder))) {
      return failure();
    }

    // The axis to which the offset must be applied need to be known.
    // However, in some cases, the pass fails to detect whether an offset should
    // be applied to an axis other than the first. We, therefore, try to find
    // out if the offset is multiplied by a known stride. Example:
    //    off += BLOCK_SIZE_K * stride_ak
    // Indeed, as the axis of the stride is known with certainty, we can assume
    // that if the offset is multiplied by a known stride, the axis of offset
    // should correspond to the axis of the stride axis. In the previous
    // example, suppose we have strides = [stride_am, stride_ak] but offsets =
    // [off, 0] As we found that `off` is multiplied by `stride_ak`, we correct
    // the axis of the offsets to align the axis of `off` with axis of
    // `stride_ak`. The corrected offsets then become: [0, off] Limitations:
    //     - this approach based on pattern matching + user code assumptions is
    //     (very) fragile.
    //       if user code does not directly multiply the offset by the stride
    //       value identified by the pass, the analysis will fail.
    //     - in theory, this correction support should fail if the analysis
    //     cannot reach a certain level of certainty.
    //       Typically, if stride values are the same (e.g. [512, 512]), the
    //       support is unable to determine the right axis and will not correct
    //       anything. That said, we do not guarantee the current support does
    //       not give rise to false positive detections.
    auto parentOp = addptrOp->getParentOp();
    if (isa<scf::ForOp>(parentOp)) {
      // ExpandOp direclty sets offset to the expected axis.
      // So if an ExpandOp has been found in defining path, the analysis is
      // skipped.
      if (!hasExpandOpInDefiningPath(addptrOp.getOffset())) {
        auto axis = checkIfOffsetMultipliedByStride(addptrOp.getOffset(),
                                                    ptrState.strides);
        if (axis >= 1)
          std::swap(offsetState.offsets[0], offsetState.offsets[axis]);
      }
    }

    assert(ptrState.source && "ptr field should provide source / base pointer");

    assert(ptrState.getRank() == offsetState.getRank() &&
           "ptr and offset field should have the same rank");

    LLVM_DEBUG(llvm::dbgs() << "Base: " << ptrState << "\n"
                            << "Offset: " << offsetState << "\n";);

    return state.addState(ptrState, offsetState, addptrOp, builder);
  }

  LogicalResult visitOperand(Value operand, PtrState &state, const Location loc,
                             OpBuilder &builder) {
    if (knownPtrs.find(operand) != knownPtrs.end()) {
      state = knownPtrs.lookup(operand);
      return success();
    }

    if (isa<IntegerType>(operand.getType())) {
      OpBuilder::InsertionGuard guard(builder);
      if (Operation *definingOp = operand.getDefiningOp())
        builder.setInsertionPointAfter(definingOp);
      auto castOp = builder.create<arith::IndexCastOp>(
          loc, builder.getIndexType(), operand);
      state.scalar = castOp.getResult();
      return success();
    }

    if (isa<IndexType>(operand.getType())) {
      state.scalar = operand;
      return success();
    }

    if (isa<triton::PointerType>(operand.getType())) {
      // A scalar pointer can either be produced by AddPtrOp or a block
      // argument
      if (Operation *op = operand.getDefiningOp()) {
        if (auto addPtrOp = dyn_cast<triton::AddPtrOp>(op))
          return visitOperandAddptr(addPtrOp, state, loc, builder);
        if (isa<triton::MakeTensorPtrOp>(op))
          llvm_unreachable(
              "Unexpected operand defining operation tt.make_tensor_ptr");
        llvm_unreachable("Unexpected operand defining operation");
      }
      state.source = operand;
      return success();
    }

    Operation *definingOp = operand.getDefiningOp();
    if (!definingOp) {
      llvm::errs() << "TritonRaiseBlockPointer: encountered addptr block "
                      "argument operand\n"
                   << operand << "\n";
    }

    return TypeSwitch<Operation *, LogicalResult>(definingOp)
        .Case<arith::AddIOp, arith::ConstantOp, arith::MulIOp, arith::RemUIOp,
              arith::RemSIOp, triton::BroadcastOp, triton::MakeRangeOp,
              triton::SplatOp, triton::ExpandDimsOp>(
            [this, &state, loc, &builder](auto op) {
              return visitAddPointerOperand(op, state, loc, builder);
            })
        .Default([](Operation *op) {
          llvm::dbgs() << "TritonRaiseBlockPointer: encountered addptr operand "
                          "produced by an unsupported operation\n"
                       << op << "\n";
          return failure();
        });
  }

  template <typename OpTy>
  LogicalResult visitAddPointerOperand(OpTy op, PtrState &state, Location loc,
                                       OpBuilder &builder);

  template <typename OpTy,
            std::enable_if_t<
                llvm::is_one_of<OpTy, arith::RemSIOp, arith::RemUIOp>::value,
                bool> = true>
  LogicalResult visitAddPointerRemOperand(OpTy remOp, PtrState &state,
                                          Location loc, OpBuilder &builder);

  template <typename OpTy,
            std::enable_if_t<
                llvm::is_one_of<OpTy, triton::LoadOp, triton::StoreOp>::value,
                bool> = true>
  LogicalResult rewriteLoadStoreOp(OpTy op) {
    constexpr bool isLoad = std::is_same_v<OpTy, triton::LoadOp>;
    constexpr StringLiteral opName =
        isLoad ? StringLiteral("loadOp") : StringLiteral("storeOp");

    Value ptr = ptrMap.lookupOrNull(op.getPtr());

    if (!ptr) {
      op->emitRemark("TritonRaiseBlockPointer: pointer is not replaced with "
                     "tt.make_tensor_ptr so ")
          << opName << " cannot be rewritten";
      return failure();
    }

    auto ptrType = dyn_cast<triton::PointerType>(ptr.getType());
    if (ptrType && !isa<ShapedType>(ptrType.getPointeeType())) {
      op->emitRemark("TritonRaiseBlockPointer: scalar ")
          << opName << " will not be rewritten";
      return failure();
    }

    // As masks are incompatible with block pointer load/store ops
    // Masks must be handled before the operation can be rewritten.
    // This will be done in a future PR (Issue #1784).
    // In the meantime, operations with a mask are not rewrtitten.
    if (op.getMask()) {
      return success();
    }

    SmallVector<int> boundary;
    if (auto iter = knownPtrs.find(ptr); iter != knownPtrs.end()) {
      auto state = iter->second;
      for (int axis = 0; axis < state.shape.size(); ++axis) {
        if (!isConstant(state.shape[axis], 0))
          boundary.push_back(axis);
      }
    }
    ArrayRef<int> newBoundaryCheck(boundary);

    OpBuilder builder(op);
    if constexpr (isLoad) {
      auto loadOp = builder.create<triton::LoadOp>(
          op.getLoc(), ptr, newBoundaryCheck, op.getPadding(), op.getCache(),
          op.getEvict(), op.getIsVolatile());

      LLVM_DEBUG(llvm::dbgs() << "creating tt.load: " << loadOp << "\n";);

      op.replaceAllUsesWith(loadOp.getResult());
    } else {
      [[maybe_unused]] auto storeOp = builder.create<triton::StoreOp>(
          op.getLoc(), ptr, op.getValue(), op.getBoundaryCheck(), op.getCache(),
          op.getEvict());

      LLVM_DEBUG(llvm::dbgs() << "creating tt.store: " << storeOp << "\n";);
    }

    op->erase();
    return success();
  }

  llvm::SmallDenseMap<Value, PtrState> knownPtrs;
  IRMapping ptrMap;
  IndexMapSet levelToBlockArgIndex;
  int level = 0;
};

template <
    typename OpTy,
    std::enable_if_t<
        llvm::is_one_of<OpTy, arith::RemSIOp, arith::RemUIOp>::value, bool>>
LogicalResult TritonRaiseBlockPointer::visitAddPointerRemOperand(
    OpTy remOp, PtrState &state, Location loc, OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  PtrState rhsState;
  if (failed(visitOperand(remOp.getRhs(), rhsState, loc, builder))) {
    return failure();
  }

  if (!rhsState.scalar) {
    remOp->emitRemark(
        "TritonRaiseBlockPointer: only support cases when rhs of remainder "
        "contains scalar");
    return failure();
  }

  if (failed(visitOperand(remOp.getLhs(), state, loc, builder))) {
    return failure();
  }

  // If there are multiple modulo ops on an expression (e.g.: (a % b) % c), we
  // would have already populated the modulo states after visiting the lhs.
  // Assert that all the modulo states are empty.
  if (state.hasModulo()) {
    remOp->emitRemark("TritonRaiseBlockPointer: do not support multiple modulo "
                      "within an expression");
    return failure();
  }

  switch (state.getRank()) {
  case 1:
    // Apply the modulo before expanding shape, the common pattern is
    // offs_am = (pid_m * BLOCK_SIZE_M + tl.arange(0, BLOCK_SIZE_M)) % M
    // a_ptrs = a_ptr + (offs_am[:, None] * stride_am + offs_k[None, :] *
    // stride_ak)
    state.shape.back() = rhsState.scalar;
    break;
  case 2: {
    // torch inductor expands the tensor shape before applying the modulo.
    //
    // We only support either:
    // - (tl.arange(0, end)[:, None] % mod), or
    // - (tl.arange(0, end)[None, :] % mod)
    //
    // In both cases, we apply the modulo to the non-singleton dimension.
    auto shape = cast<TensorType>(remOp.getResult().getType()).getShape();
    if (shape[0] == 1) {
      state.shape[1] = rhsState.scalar;
    } else if (shape[1] == 1) {
      state.shape[0] = rhsState.scalar;
    } else {
      remOp->emitRemark("TritonRaiseBlockPointer: taking modulo on a 2D tensor "
                        "with no singleton dimension not supported");
      return failure();
    }
    break;
  }
  default:
    remOp->emitRemark("TritonRaiseBlockPointer: unsupported modulo pattern");
    return failure();
  }

  return success();
}

template <>
LogicalResult TritonRaiseBlockPointer::visitAddPointerOperand(
    arith::RemSIOp remOp, PtrState &state, Location loc, OpBuilder &builder) {
  return visitAddPointerRemOperand(remOp, state, loc, builder);
}

template <>
LogicalResult TritonRaiseBlockPointer::visitAddPointerOperand(
    arith::RemUIOp remOp, PtrState &state, Location loc, OpBuilder &builder) {
  return visitAddPointerRemOperand(remOp, state, loc, builder);
}

template <>
LogicalResult
TritonRaiseBlockPointer::visitAddPointerOperand(triton::MakeRangeOp rangeOp,
                                                PtrState &state, Location loc,
                                                OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  ArrayRef<int64_t> shape = cast<ShapedType>(rangeOp.getType()).getShape();

  uint32_t start = rangeOp.getStart();
  uint32_t end = rangeOp.getEnd();
  uint32_t stride = (end - start + shape[0] - 1) / shape[0];
  assert(stride == 1 &&
         "Expect make_range op to always return tensor of stride 1");

  state.offsets.push_back(
      builder.create<arith::ConstantIntOp>(loc, start, offsetBitwidth));
  state.strides.push_back(builder.create<arith::ConstantIntOp>(
      loc, stride, shapeAndStridesBitwidth));
  state.shape.push_back(
      builder.create<arith::ConstantIntOp>(loc, 0, shapeAndStridesBitwidth));
  state.sizes.push_back(shape[0]);

  LLVM_DEBUG(llvm::dbgs() << "MakeRange state: " << state << "\n";);

  return success();
}

template <>
LogicalResult
TritonRaiseBlockPointer::visitAddPointerOperand(triton::SplatOp splatOp,
                                                PtrState &state, Location loc,
                                                OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  Value src = splatOp.getSrc();
  Value dst = splatOp.getResult();
  ArrayRef<int64_t> dstShape = cast<ShapedType>(dst.getType()).getShape();

  if (failed(visitOperand(src, state, loc, builder)))
    return failure();

  if (!isa<IntegerType, IndexType, triton::PointerType>(src.getType())) {
    splatOp->emitRemark("TritonRaiseBlockPointer: unsupported splat pattern");
    return failure();
  }

  for (int64_t s : dstShape) {
    Value c0i32 = builder.create<arith::ConstantIntOp>(loc, 0, offsetBitwidth);
    Value c0i64 =
        builder.create<arith::ConstantIntOp>(loc, 0, shapeAndStridesBitwidth);
    state.offsets.push_back(c0i32);
    state.strides.push_back(c0i64);
    state.shape.push_back(c0i64);
    state.sizes.push_back(s);
  }

  // If we splat a integer value, scalar should become the offset of the
  // outer most dimension
  if (state.scalar) {
    state.offsets[0] = getValueOrCreateCastToIndexLike(
        builder, loc, builder.getIntegerType(offsetBitwidth), state.scalar);
  }

  LLVM_DEBUG(llvm::dbgs() << "Splat state: " << state << "\n";);

  return success();
}

template <>
LogicalResult TritonRaiseBlockPointer::visitAddPointerOperand(
    arith::AddIOp addOp, PtrState &state, Location loc, OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  PtrState lhsState;
  if (failed(visitOperand(addOp.getLhs(), lhsState, loc, builder)))
    return failure();

  PtrState rhsState;
  if (failed(visitOperand(addOp.getRhs(), rhsState, loc, builder)))
    return failure();

  if (failed(state.addState(lhsState, rhsState, addOp, builder)))
    return failure();

  LLVM_DEBUG(llvm::dbgs() << "Add state: " << state << "\n";);

  return success();
}

template <>
LogicalResult TritonRaiseBlockPointer::visitAddPointerOperand(
    arith::MulIOp mulOp, PtrState &state, Location loc, OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  PtrState lhsState;
  if (failed(visitOperand(mulOp.getLhs(), lhsState, loc, builder)))
    return failure();

  PtrState rhsState;
  if (failed(visitOperand(mulOp.getRhs(), rhsState, loc, builder)))
    return failure();

  if (failed(state.mulState(lhsState, rhsState, mulOp, builder)))
    return failure();

  LLVM_DEBUG(llvm::dbgs() << "Mul state: " << state << "\n";);

  return success();
}

template <>
LogicalResult TritonRaiseBlockPointer::visitAddPointerOperand(
    arith::ConstantOp op, PtrState &state, Location loc, OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  auto attr = cast<DenseElementsAttr>(op.getValue());
  Type elementType = attr.getElementType();
  assert(attr.isSplat() && isa<IntegerType>(elementType) &&
         "Expecting constant tensor");

  state.scalar = builder.create<arith::ConstantIndexOp>(
      loc, attr.getValues<IntegerAttr>()[0].getValue().getSExtValue());

  Type offsetType = builder.getIntegerType(offsetBitwidth);
  auto resultType = cast<ShapedType>(op.getResult().getType());
  Value offset = convertScalarToDtype(builder, loc, state.scalar, offsetType,
                                      /*isUnsignedCast=*/true);
  state.offsets.push_back(offset);
  state.offsets.insert(
      state.offsets.end(), resultType.getShape().size() - 1,
      builder.create<arith::ConstantIntOp>(loc, 0, offsetBitwidth));
  state.strides.insert(
      state.strides.end(), resultType.getShape().size(),
      builder.create<arith::ConstantIntOp>(loc, 0, shapeAndStridesBitwidth));
  state.shape.insert(
      state.shape.end(), resultType.getShape().size(),
      builder.create<arith::ConstantIntOp>(loc, 0, shapeAndStridesBitwidth));

  for (int32_t dim : resultType.getShape()) {
    state.sizes.push_back(dim);
  }

  return success();
}

template <>
LogicalResult TritonRaiseBlockPointer::visitAddPointerOperand(
    triton::ExpandDimsOp expandDimsOp, PtrState &state, Location loc,
    OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  if (failed(visitOperand(expandDimsOp.getSrc(), state, loc, builder))) {
    return failure();
  }

  ArrayRef<int64_t> dstShape =
      cast<ShapedType>(expandDimsOp.getResult().getType()).getShape();
  auto axis = expandDimsOp.getAxis();

  assert(dstShape[axis] == 1 &&
         "expect changed dimension to be 1 in expand_dims");

  // insert dimension info
  Value c0i32 = builder.create<arith::ConstantIntOp>(loc, 0, offsetBitwidth);
  Value c0i64 =
      builder.create<arith::ConstantIntOp>(loc, 0, shapeAndStridesBitwidth);
  state.offsets.insert(state.offsets.begin() + axis, c0i32);
  state.sizes.insert(state.sizes.begin() + axis, 1);
  state.strides.insert(state.strides.begin() + axis, c0i64);
  state.shape.insert(state.shape.begin() + axis, c0i64);

  if (state.hasModulo() && state.getRank() > 2) {
    expandDimsOp->emitRemark("TritonRaiseBlockPointer: unsupported scenario "
                             "where expand_dims result "
                             "has modulo and rank > 2");
    return failure();
  }

  LLVM_DEBUG(llvm::dbgs() << "ExpandDims state: " << state << "\n";);

  return success();
}

template <>
LogicalResult
TritonRaiseBlockPointer::visitAddPointerOperand(triton::BroadcastOp broadcastOp,
                                                PtrState &state, Location loc,
                                                OpBuilder &builder) {
  assert(state.isEmpty() && "state is a return argument");

  Value src = broadcastOp.getSrc();
  Value dst = broadcastOp.getResult();

  if (!isa<ShapedType>(src.getType())) {
    broadcastOp->emitRemark(
        "TritonRaiseBlockPointer: Unsupported broadcast source type");
    return failure();
  }

  ArrayRef<int64_t> srcShape = cast<ShapedType>(src.getType()).getShape();
  ArrayRef<int64_t> dstShape = cast<ShapedType>(dst.getType()).getShape();

  assert(srcShape.size() <= dstShape.size() &&
         "rank of source cannot be greater than the rank of destination");

  if (failed(visitOperand(src, state, loc, builder))) {
    return failure();
  }

  if (srcShape.size() == dstShape.size()) {
    llvm::copy(dstShape, state.sizes.begin());
  } else {
    // Offset must be equal, otherwise we don.t know which offset should be
    // propagated to the new axis.
    for (int i = 1; i < state.offsets.size(); ++i) {
      if (state.offsets[0] != state.offsets[i]) {
        broadcastOp->emitRemark(
            "TritonRaiseBlockPointer: Unsupported broadcast with different "
            "offsets while source rank and destination rank differ.");
        return failure();
      }
    }

    // Create the new axis.
    // The positions of the new axis are determined based and the shape values.
    // If shape are the same, the new axis are added at the end.
    size_t srcAxis = 0;
    for (size_t axis = 0; axis < dstShape.size(); ++axis) {
      if ((srcAxis < srcShape.size()) &&
          (srcShape[srcAxis] == dstShape[axis])) {
        ++srcAxis;
        continue;
      }
      Value c0i32 =
          builder.create<arith::ConstantIntOp>(loc, 0, offsetBitwidth);
      Value c0i64 =
          builder.create<arith::ConstantIntOp>(loc, 0, shapeAndStridesBitwidth);
      state.offsets.insert(state.offsets.begin() + axis,
                           getValueOrCreateCastToIndexLike(
                               builder, loc,
                               builder.getIntegerType(offsetBitwidth),
                               state.offsets[0]));
      state.sizes.insert(state.sizes.begin() + axis, dstShape[axis]);
      state.strides.insert(state.strides.begin() + axis, c0i64);
      state.shape.insert(state.shape.begin() + axis, c0i64);
    }

    // The following condition has been duplicated from the expand_dim support
    // TODO : Verify if we need still need it given that triton `make_block_ptr`
    // op differs from triton-shared `make_block_ptr` op regarding how address
    // wrap around are handled.
    if (state.hasModulo() && state.getRank() > 2) {
      broadcastOp->emitRemark("TritonRaiseBlockPointer: unsupported scenario "
                              "where broadcast result "
                              "has modulo and rank > 2");
      return failure();
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "Broadcast state: " << state << "\n";);

  return success();
}
} // namespace
