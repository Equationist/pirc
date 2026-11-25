-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

-- Pure-Ada Ada 83 Program Library Metadata & Binder (LRM Chapter 10)
-- Extracts compilation unit metadata directly from attributed DIANA ASTs
-- into <lib_dir>/UNITS.IDX and performs LRM 10.3 / 10.5 binding,
-- obsolescence checking, cycle detection, live .o selection, and
-- binder_main.pir elaboration generation.

with USERPK; use USERPK;

package Lib_Binder is

   -- Deterministic task-group ID (>= 16#4000_0000#) for a library package name
   -- so both diana2pir and ada83_bind agree on spec/body task activation groups.
   function Lib_Pkg_Task_Group_Id (Name : String) return Long_Long_Integer;

   -- Record all compilation units from an attributed DIANA Comp_Node into
   -- <Lib_Dir>/UNITS.IDX, associating them with Obj_Path.
   procedure Record_Compilation_Units
     (Comp_Node : TREE;
      Obj_Path  : String;
      Lib_Dir   : String := "ADA__LIB");

   -- Bind a program library (<Lib_Dir>/UNITS.IDX):
   --   * Performs LRM 10.3 missing/obsolete body, subunit, and withed-unit checks
   --   * Performs LRM 10.5 circular elaboration dependency detection
   --   * Selects live (non-superseded, non-obsoleted) .o files -> Objs_Out_Path
   --   * Emits binder_main.pir with topological elaboration order -> Pir_Out_Path
   -- Returns True on success; on failure writes a diagnostic to Err_Msg (and stderr).
   procedure Bind_Library
     (Lib_Dir       : String;
      Main_Unit     : String;
      Target_Triple : String;
      Pir_Out_Path  : String;
      Objs_Out_Path : String;
      Check_Only    : Boolean;
      Skip_Checks   : Boolean;
      Success       : out Boolean;
      Main_Obj      : String := "");

end Lib_Binder;
