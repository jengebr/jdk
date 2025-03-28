/*
 * Copyright (c) 2016, 2025, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2016 SAP SE. All rights reserved.
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

#include "asm/assembler.inline.hpp"
#include "compiler/disassembler.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "interpreter/interpreter.hpp"
#include "gc/shared/cardTableBarrierSet.hpp"
#include "memory/resourceArea.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/os.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "utilities/macros.hpp"

// Convention: Use Z_R0 and Z_R1 instead of Z_scratch_* in all
// assembler_s390.* files.

// Convert the raw encoding form into the form expected by the
// constructor for Address. This is called by adlc generated code.
Address Address::make_raw(int base, int index, int scale, int disp, relocInfo::relocType disp_reloc) {
  assert(scale == 0, "Scale should not be used on z/Architecture. The call to make_raw is "
         "generated by adlc and this must mirror all features of Operands from machnode.hpp.");
  assert(disp_reloc == relocInfo::none, "not implemented on z/Architecture.");

  Address madr(as_Register(base), as_Register(index), in_ByteSize(disp));
  return madr;
}

int AbstractAssembler::code_fill_byte() {
  return 0x00; // Illegal instruction 0x00000000.
}

// Condition code masks. Details see enum branch_condition.
// Although this method is meant for INT CCs, the Overflow/Ordered
// bit in the masks has to be considered. The CC might have been set
// by a float operation, but is evaluated while calculating an integer
// result. See elementary test TestFloat.isNotEqual(FF)Z for example.
Assembler::branch_condition Assembler::inverse_condition(Assembler::branch_condition cc) {
  Assembler::branch_condition unordered_bit = (Assembler::branch_condition)(cc & bcondNotOrdered);
  Assembler::branch_condition inverse_cc;

  // Some are commented out to avoid duplicate labels.
  switch (cc) {
    case bcondNever       : inverse_cc = bcondAlways;      break;  //  0 -> 15
    case bcondAlways      : inverse_cc = bcondNever;       break;  // 15 ->  0

    case bcondOverflow    : inverse_cc = bcondNotOverflow; break;  //  1 -> 14
    case bcondNotOverflow : inverse_cc = bcondOverflow;    break;  // 14 ->  1

    default :
      switch ((Assembler::branch_condition)(cc & bcondOrdered)) {
        case bcondEqual       : inverse_cc = bcondNotEqual;  break;  //  8 ->  6
        // case bcondZero        :
        // case bcondAllZero     :

        case bcondNotEqual    : inverse_cc = bcondEqual;     break;  //  6 ->  8
        // case bcondNotZero     :
        // case bcondMixed       :

        case bcondLow         : inverse_cc = bcondNotLow;    break;  //  4 -> 10
        // case bcondNegative    :

        case bcondNotLow      : inverse_cc = bcondLow;       break;  // 10 ->  4
        // case bcondNotNegative :

        case bcondHigh        : inverse_cc = bcondNotHigh;   break;  //  2 -> 12
        // case bcondPositive    :

        case bcondNotHigh     : inverse_cc = bcondHigh;      break;  // 12 ->  2
        // case bcondNotPositive :

        default :
          fprintf(stderr, "inverse_condition(%d)\n", (int)cc);
          fflush(stderr);
          ShouldNotReachHere();
          return bcondNever;
      }
      // If cc is even, inverse_cc must be odd.
      if (!unordered_bit) {
        inverse_cc = (Assembler::branch_condition)(inverse_cc | bcondNotOrdered);
      }
      break;
  }
  return inverse_cc;
}

Assembler::branch_condition Assembler::inverse_float_condition(Assembler::branch_condition cc) {
  Assembler::branch_condition  inverse_cc;

  switch (cc) {
    case bcondNever       : inverse_cc = bcondAlways;      break;  //  0
    case bcondAlways      : inverse_cc = bcondNever;       break;  // 15

    case bcondNotOrdered  : inverse_cc = bcondOrdered;     break;  // 14
    case bcondOrdered     : inverse_cc = bcondNotOrdered;  break;  //  1

    case bcondEqual                : inverse_cc = bcondNotEqualOrNotOrdered; break;  //  8
    case bcondNotEqualOrNotOrdered : inverse_cc = bcondEqual;                break;  //  7

    case bcondLowOrNotOrdered      : inverse_cc = bcondNotLow;               break;  //  5
    case bcondNotLow               : inverse_cc = bcondLowOrNotOrdered;      break;  // 10

    case bcondHigh                 : inverse_cc = bcondNotHighOrNotOrdered;  break;  //  2
    case bcondNotHighOrNotOrdered  : inverse_cc = bcondHigh;                 break;  // 13

    default :
      fprintf(stderr, "inverse_float_condition(%d)\n", (int)cc);
      fflush(stderr);
      ShouldNotReachHere();
      return bcondNever;
  }
  return inverse_cc;
}

#ifndef PRODUCT
void Assembler::print_dbg_msg(outputStream* out, unsigned long inst, const char* msg, int ilen) {
  out->flush();
  switch (ilen) {
    case 2:  out->print_cr("inst = %4.4x, %s",    (unsigned short)inst, msg); break;
    case 4:  out->print_cr("inst = %8.8x, %s\n",    (unsigned int)inst, msg); break;
    case 6:  out->print_cr("inst = %12.12lx, %s\n",               inst, msg); break;
    default: out->print_cr("inst = %16.16lx, %s\n",               inst, msg); break;
  }
  out->flush();
}

void Assembler::dump_code_range(outputStream* out, address pc, const unsigned int range, const char* msg) {
  out->cr();
  out->print_cr("-------------------------------");
  out->print_cr("--  %s", msg);
  out->print_cr("-------------------------------");
  out->print_cr("Hex dump    of +/-%d bytes around %p, interval [%p,%p)", range, pc, pc-range, pc+range);
  os::print_hex_dump(out, pc-range, pc+range, 2);

  out->cr();
  out->print_cr("Disassembly of +/-%d bytes around %p, interval [%p,%p)", range, pc, pc-range, pc+range);
  Disassembler::decode(pc, pc + range, out);
}
#endif
