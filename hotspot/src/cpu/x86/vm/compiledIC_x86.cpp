/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
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
#include "asm/macroAssembler.inline.hpp"
#include "code/compiledIC.hpp"
#include "code/icBuffer.hpp"
#include "code/nmethod.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"

// Release the CompiledICHolder* associated with this call site is there is one.
void CompiledIC::cleanup_call_site(virtual_call_Relocation* call_site) {
  // This call site might have become stale so inspect it carefully.
  NativeCall* call = nativeCall_at(call_site->addr());
  if (is_icholder_entry(call->destination())) {
    NativeMovConstReg* value = nativeMovConstReg_at(call_site->cached_value());
    InlineCacheBuffer::queue_for_release((CompiledICHolder*)value->data());
  }
}

bool CompiledIC::is_icholder_call_site(virtual_call_Relocation* call_site) {
  // This call site might have become stale so inspect it carefully.
  NativeCall* call = nativeCall_at(call_site->addr());
  return is_icholder_entry(call->destination());
}

// ----------------------------------------------------------------------------

#define __ _masm.
address CompiledStaticCall::emit_to_interp_stub(CodeBuffer &cbuf, address mark) {
  // Stub is fixed up when the corresponding call is converted from
  // calling compiled code to calling interpreted code.
  // movq rbx, 0
  // jmp -5 # to self

  if (mark == NULL) {
    mark = cbuf.insts_mark();  // Get mark within main instrs section.
  }

  // Note that the code buffer's insts_mark is always relative to insts.
  // That's why we must use the macroassembler to generate a stub.
  MacroAssembler _masm(&cbuf);

  address base = __ start_a_stub(to_interp_stub_size());
  if (base == NULL) {
    return NULL;  // CodeBuffer::expand failed.
  }
  // Static stub relocation stores the instruction address of the call.
  __ relocate(static_stub_Relocation::spec(mark), Assembler::imm_operand);
  // Static stub relocation also tags the Method* in the code-stream.
  __ mov_metadata(rbx, (Metadata*) NULL);  // Method is zapped till fixup time.
  // This is recognized as unresolved by relocs/nativeinst/ic code.
  __ jump(RuntimeAddress(__ pc()));

  assert(__ pc() - base <= to_interp_stub_size(), "wrong stub size");

  // Update current stubs pointer and restore insts_end.
  __ end_a_stub();
  return base;
}
#undef __

int CompiledStaticCall::to_interp_stub_size() {
  return NOT_LP64(10)    // movl; jmp
         LP64_ONLY(15);  // movq (1+1+8); jmp (1+4)
}

// Relocation entries for call stub, compiled java to interpreter.
int CompiledStaticCall::reloc_to_interp_stub() {
  return 4; // 3 in emit_to_interp_stub + 1 in emit_call
}

void CompiledStaticCall::set_to_interpreted(methodHandle callee, address entry) {
  address stub = find_stub();
  guarantee(stub != NULL, "stub not found");

  if (TraceICs) {
    ResourceMark rm;
    tty->print_cr("CompiledStaticCall@" INTPTR_FORMAT ": set_to_interpreted %s",
                  p2i(instruction_address()),
                  callee->name_and_sig_as_C_string());
  }

  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub);
  NativeJump*        jump          = nativeJump_at(method_holder->next_instruction_address());

#ifdef ASSERT
  // read the value once
  intptr_t data = method_holder->data();
  address destination = jump->jump_destination();
  assert(data == 0 || data == (intptr_t)callee(),
         "a) MT-unsafe modification of inline cache");
  assert(destination == (address)-1 || destination == entry,
         "b) MT-unsafe modification of inline cache");
#endif

  // Update stub.
  method_holder->set_data((intptr_t)callee());
  jump->set_jump_destination(entry);

  // Update jump to call.
  set_destination_mt_safe(stub);
}

void CompiledStaticCall::set_stub_to_clean(static_stub_Relocation* static_stub) {
  assert (CompiledIC_lock->is_locked() || SafepointSynchronize::is_at_safepoint(), "mt unsafe call");
  // Reset stub.
  address stub = static_stub->addr();
  assert(stub != NULL, "stub not found");
  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub);
  method_holder->set_data(0);
  NativeJump* jump = nativeJump_at(method_holder->next_instruction_address());
  jump->set_jump_destination((address)-1);
}


//-----------------------------------------------------------------------------
// Non-product mode code
#ifndef PRODUCT

void CompiledStaticCall::verify() {
  // Verify call.
  NativeCall::verify();
  if (os::is_MP()) {
    verify_alignment();
  }

  // Verify stub.
  address stub = find_stub();
  assert(stub != NULL, "no stub found for static call");
  // Creation also verifies the object.
  NativeMovConstReg* method_holder = nativeMovConstReg_at(stub);
  NativeJump*        jump          = nativeJump_at(method_holder->next_instruction_address());

  // Verify state.
  assert(is_clean() || is_call_to_compiled() || is_call_to_interpreted(), "sanity check");
}
#endif // !PRODUCT