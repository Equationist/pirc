-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Stmt.Subps is

   procedure Emit_Subprogram_Body
     (Ctx           : in out Context;
      Body_Node     : TREE;
      Override_Id   : TREE := CONST_VOID;
      Override_Spec : TREE := CONST_VOID;
      Is_Exported   : Boolean := False;
      Is_Subunit    : Boolean := False);

   procedure Emit_Task_Body
     (Ctx       : in out Context;
      Body_Node : TREE);

   procedure Emit_Compilation
     (Ctx       : in out Context;
      Comp_Node : TREE;
      Only_CU   : Natural := 0);

end Cg_Stmt.Subps;
