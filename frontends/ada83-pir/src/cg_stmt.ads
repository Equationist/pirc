-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with USERPK; use USERPK;
with Cg_State; use Cg_State;

package Cg_Stmt is

   -- Emit a complete DIANA compilation node (all compilation units, or only Only_CU when > 0)
   Elab_Cycle_Error : exception;

   procedure Emit_Compilation (Ctx : in out Context; Comp_Node : TREE; Only_CU : Natural := 0);

end Cg_Stmt;
