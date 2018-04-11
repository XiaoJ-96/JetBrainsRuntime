/*
 * Copyright (c) 2018, Red Hat, Inc. and/or its affiliates.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "gc/shenandoah/brooksPointer.hpp"
#include "gc/shenandoah/shenandoahConnectionMatrix.hpp"
#include "gc/shenandoah/shenandoahAsserts.hpp"
#include "gc/shenandoah/shenandoahHeap.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "memory/resourceArea.hpp"

void ShenandoahAsserts::print_obj(ShenandoahMessageBuffer& msg, oop obj) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahHeapRegion *r = heap->heap_region_containing(obj);

  ResourceMark rm;
  stringStream ss;
  r->print_on(&ss);

  msg.append("  " PTR_FORMAT " - klass " PTR_FORMAT " %s\n", p2i(obj), p2i(obj->klass()), obj->klass()->external_name());
  msg.append("    %3s allocated after complete mark start\n", heap->allocated_after_complete_mark_start((HeapWord *) obj) ? "" : "not");
  msg.append("    %3s allocated after next mark start\n",     heap->allocated_after_next_mark_start((HeapWord *) obj)     ? "" : "not");
  msg.append("    %3s marked complete\n",      heap->is_marked_complete(obj) ? "" : "not");
  msg.append("    %3s marked next\n",          heap->is_marked_next(obj) ? "" : "not");
  msg.append("    %3s in collection set\n",    heap->in_collection_set(obj) ? "" : "not");
  msg.append("  region: %s", ss.as_string());
}

void ShenandoahAsserts::print_non_obj(ShenandoahMessageBuffer& msg, void* loc) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (heap->is_in(loc)) {
    msg.append("  inside Java heap\n");
    ShenandoahHeapRegion *r = heap->heap_region_containing(loc);
    stringStream ss;
    r->print_on(&ss);

    msg.append("    %3s in collection set\n",    heap->in_collection_set(loc) ? "" : "not");
    msg.append("  region: %s", ss.as_string());
  } else {
    msg.append("  outside of Java heap\n");
    stringStream ss;
    os::print_location(&ss, (intptr_t) loc, false);
    msg.append("  %s", ss.as_string());
  }
}

void ShenandoahAsserts::print_obj_safe(ShenandoahMessageBuffer& msg, void* loc) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  msg.append("  " PTR_FORMAT " - safe print, no details\n", p2i(loc));
  if (heap->is_in(loc)) {
    ShenandoahHeapRegion* r = heap->heap_region_containing(loc);
    if (r != NULL) {
      stringStream ss;
      r->print_on(&ss);
      msg.append("  region: %s", ss.as_string());
    }
  }
}

void ShenandoahAsserts::print_failure(SafeLevel level, oop obj, void* interior_loc, oop loc,
                                       const char* phase, const char* label,
                                       const char* file, int line) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ResourceMark rm;

  bool loc_in_heap = (loc != NULL && heap->is_in(loc));
  bool interior_loc_in_heap = (interior_loc != NULL && heap->is_in(interior_loc));

  ShenandoahMessageBuffer msg("%s; %s\n\n", phase, label);

  msg.append("Referenced from:\n");
  if (interior_loc != NULL) {
    msg.append("  interior location: " PTR_FORMAT "\n", p2i(interior_loc));
    if (loc_in_heap) {
      print_obj(msg, loc);
    } else {
      print_non_obj(msg, interior_loc);
    }
  } else {
    msg.append("  no interior location recorded (probably a plain heap scan, or detached oop)\n");
  }
  msg.append("\n");

  msg.append("Object:\n");
  if (level >= _safe_oop) {
    print_obj(msg, obj);
  } else {
    print_obj_safe(msg, obj);
  }
  msg.append("\n");

  if (level >= _safe_oop) {
    oop fwd = (oop) BrooksPointer::get_raw_unchecked(obj);
    msg.append("Forwardee:\n");
    if (!oopDesc::unsafe_equals(obj, fwd)) {
      if (level >= _safe_oop_fwd) {
        print_obj(msg, fwd);
      } else {
        print_obj_safe(msg, fwd);
      }
    } else {
      msg.append("  (the object itself)");
    }
    msg.append("\n");
  }

  if (level >= _safe_oop_fwd) {
    oop fwd = (oop) BrooksPointer::get_raw_unchecked(obj);
    oop fwd2 = (oop) BrooksPointer::get_raw_unchecked(fwd);
    if (!oopDesc::unsafe_equals(fwd, fwd2)) {
      msg.append("Second forwardee:\n");
      print_obj_safe(msg, fwd2);
      msg.append("\n");
    }
  }

  if (loc_in_heap && UseShenandoahMatrix && (level == _safe_all)) {
    msg.append("Matrix connections:\n");

    oop fwd_to = (oop) BrooksPointer::get_raw_unchecked(obj);
    oop fwd_from = (oop) BrooksPointer::get_raw_unchecked(loc);

    size_t from_idx = heap->heap_region_index_containing(loc);
    size_t to_idx = heap->heap_region_index_containing(obj);
    size_t fwd_from_idx = heap->heap_region_index_containing(fwd_from);
    size_t fwd_to_idx = heap->heap_region_index_containing(fwd_to);

    ShenandoahConnectionMatrix* matrix = heap->connection_matrix();
    msg.append("  %35s %3s connected\n",
               "reference and object",
               matrix->is_connected(from_idx, to_idx) ? "" : "not");
    msg.append("  %35s %3s connected\n",
               "fwd(reference) and object",
               matrix->is_connected(fwd_from_idx, to_idx) ? "" : "not");
    msg.append("  %35s %3s connected\n",
               "reference and fwd(object)",
               matrix->is_connected(from_idx, fwd_to_idx) ? "" : "not");
    msg.append("  %35s %3s connected\n",
               "fwd(reference) and fwd(object)",
               matrix->is_connected(fwd_from_idx, fwd_to_idx) ? "" : "not");

    if (interior_loc_in_heap) {
      size_t from_interior_idx = heap->heap_region_index_containing(interior_loc);
      msg.append("  %35s %3s connected\n",
                 "interior-reference and object",
                 matrix->is_connected(from_interior_idx, to_idx) ? "" : "not");
      msg.append("  %35s %3s connected\n",
                 "interior-reference and fwd(object)",
                 matrix->is_connected(from_interior_idx, fwd_to_idx) ? "" : "not");
    }
  }

  report_vm_error(file, line, msg.buffer());
}

void ShenandoahAsserts::assert_in_heap(void* interior_loc, oop obj, const char *file, int line) {
  ShenandoahHeap* heap = ShenandoahHeap::heap_no_check();

  if (!heap->is_in(obj)) {
    print_failure(_safe_unknown, obj, interior_loc, NULL, "Shenandoah assert_in_heap failed",
                  "oop must point to a heap address",
                  file, line);
  }
}

void ShenandoahAsserts::assert_correct(void* interior_loc, oop obj, const char* file, int line) {
  ShenandoahHeap* heap = ShenandoahHeap::heap_no_check();

  // Step 1. Check that both obj and its fwdptr are in heap.
  // After this step, it is safe to call heap_region_containing().
  if (!heap->is_in(obj)) {
    print_failure(_safe_unknown, obj, interior_loc, NULL, "Shenandoah assert_correct failed",
                  "oop must point to a heap address",
                  file, line);
  }

  oop fwd = oop(BrooksPointer::get_raw_unchecked(obj));

  if (!heap->is_in(fwd)) {
    print_failure(_safe_oop, obj, interior_loc, NULL, "Shenandoah assert_correct failed",
                  "Forwardee must point to a heap address",
                  file, line);
  }

  bool is_forwarded = !oopDesc::unsafe_equals(obj, fwd);

  // When Full GC moves the objects, we cannot trust fwdptrs. If we got here, it means something
  // tries fwdptr manipulation when Full GC is running. The only exception is using the fwdptr
  // that still points to the object itself.

  if (is_forwarded && heap->is_full_gc_move_in_progress()) {
    print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_correct failed",
                  "Non-trivial forwarding pointer during Full GC moves, probable bug.",
                  file, line);
  }

  // Step 2. Check that forwardee points to correct region.
  if (is_forwarded &&
      (heap->heap_region_index_containing(fwd) ==
       heap->heap_region_index_containing(obj))) {
    print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_correct failed",
                  "Forwardee should be self, or another region",
                  file, line);
  }

  // Step 3. Check for multiple forwardings
  if (is_forwarded) {
    oop fwd2 = oop(BrooksPointer::get_raw_unchecked(fwd));
    if (!oopDesc::unsafe_equals(fwd, fwd2)) {
      print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_correct failed",
                    "Multiple forwardings",
                    file, line);
    }
  }
}

void ShenandoahAsserts::assert_in_correct_region(void* interior_loc, oop obj, const char* file, int line) {
  assert_correct(interior_loc, obj, file, line);

  ShenandoahHeap* heap = ShenandoahHeap::heap_no_check();
  ShenandoahHeapRegion* r = heap->heap_region_containing(obj);
  if (!r->is_active()) {
    print_failure(_safe_unknown, obj, interior_loc, NULL, "Shenandoah assert_in_correct_region failed",
                  "Object must reside in active region",
                  file, line);
  }

  size_t alloc_size = obj->size() + BrooksPointer::word_size();
  if (alloc_size > ShenandoahHeapRegion::humongous_threshold_words()) {
    size_t idx = r->region_number();
    size_t num_regions = ShenandoahHeapRegion::required_regions(alloc_size * HeapWordSize);
    for (size_t i = idx; i < idx + num_regions; i++) {
      ShenandoahHeapRegion* chain_reg = heap->regions()->get(i);
      if (i == idx && !chain_reg->is_humongous_start()) {
        print_failure(_safe_unknown, obj, interior_loc, NULL, "Shenandoah assert_in_correct_region failed",
                      "Object must reside in humongous start",
                      file, line);
      }
      if (i != idx && !chain_reg->is_humongous_continuation()) {
        print_failure(_safe_oop, obj, interior_loc, NULL, "Shenandoah assert_in_correct_region failed",
                      "Humongous continuation should be of proper size",
                      file, line);
      }
    }
  }
}

void ShenandoahAsserts::assert_forwarded(void* interior_loc, oop obj, const char* file, int line) {
  assert_correct(interior_loc, obj, file, line);
  oop fwd = oop(BrooksPointer::get_raw_unchecked(obj));

  if (oopDesc::unsafe_equals(obj, fwd)) {
    print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_forwarded failed",
                  "Object should be forwarded",
                  file, line);
  }
}

void ShenandoahAsserts::assert_not_forwarded(void* interior_loc, oop obj, const char* file, int line) {
  assert_correct(interior_loc, obj, file, line);
  oop fwd = oop(BrooksPointer::get_raw_unchecked(obj));

  if (!oopDesc::unsafe_equals(obj, fwd)) {
    print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_not_forwarded failed",
                  "Object should not be forwarded",
                  file, line);
  }
}

void ShenandoahAsserts::assert_marked_complete(void* interior_loc, oop obj, const char* file, int line) {
  assert_correct(interior_loc, obj, file, line);

  ShenandoahHeap* heap = ShenandoahHeap::heap_no_check();
  if (!heap->is_marked_complete(obj)) {
    print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_marked_complete failed",
                  "Object should be marked (complete)",
                  file, line);
  }
}

void ShenandoahAsserts::assert_marked_next(void* interior_loc, oop obj, const char* file, int line) {
  assert_correct(interior_loc, obj, file, line);

  ShenandoahHeap* heap = ShenandoahHeap::heap_no_check();
  if (!heap->is_marked_next(obj)) {
    print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_marked_next failed",
                  "Object should be marked (next)",
                  file, line);
  }
}

void ShenandoahAsserts::assert_not_in_cset(void* interior_loc, oop obj, const char* file, int line) {
  assert_correct(interior_loc, obj, file, line);

  ShenandoahHeap* heap = ShenandoahHeap::heap_no_check();
  if (heap->in_collection_set(obj)) {
    print_failure(_safe_all, obj, interior_loc, NULL, "Shenandoah assert_not_in_cset failed",
                  "Object should not be in collection set",
                  file, line);
  }
}

void ShenandoahAsserts::assert_not_in_cset_loc(void* interior_loc, const char* file, int line) {
  ShenandoahHeap* heap = ShenandoahHeap::heap_no_check();
  if (heap->in_collection_set(interior_loc)) {
    print_failure(_safe_unknown, NULL, interior_loc, NULL, "Shenandoah assert_not_in_cset_loc failed",
                  "Interior location should not be in collection set",
                  file, line);
  }
}

void ShenandoahAsserts::print_rp_failure(const char *label, BoolObjectClosure* actual, BoolObjectClosure* expected,
                                         const char *file, int line) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ShenandoahMessageBuffer msg("%s\n", label);
  msg.append(" Actual:                  " PTR_FORMAT "\n", p2i(actual));
  msg.append(" Expected:                " PTR_FORMAT "\n", p2i(expected));
  msg.append(" SH->_is_alive:           " PTR_FORMAT "\n", p2i(&heap->_is_alive));
  msg.append(" SH->_forwarded_is_alive: " PTR_FORMAT "\n", p2i(&heap->_forwarded_is_alive));
  report_vm_error(file, line, msg.buffer());
}

void ShenandoahAsserts::assert_rp_isalive_not_installed(const char *file, int line) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ReferenceProcessor* rp = heap->ref_processor();
  if (rp->is_alive_non_header() != NULL) {
    print_rp_failure("Shenandoah assert_rp_isalive_not_installed failed", rp->is_alive_non_header(), NULL,
                     file, line);
  }
}

void ShenandoahAsserts::assert_rp_isalive_installed(const char *file, int line) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  ReferenceProcessor* rp = heap->ref_processor();
  if (rp->is_alive_non_header() != heap->is_alive_closure()) {
    print_rp_failure("Shenandoah assert_rp_isalive_installed failed", rp->is_alive_non_header(), heap->is_alive_closure(),
                     file, line);
  }
}
