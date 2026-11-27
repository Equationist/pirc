-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Expr.Call is

   function Compute_Call_SL (Ctx : in out Context; Callee_SI : Subp_Info) return String;

   function Emit_Subp_Call
     (Ctx       : in out Context;
      Call_Node : TREE;
      Is_Func   : Boolean) return Expr_Val;

end Cg_Expr.Call;
