-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Expr.RTL is

   function Try_Emit_RTL_Call
     (Ctx        : in out Context;
      Def        : TREE;
      Param_List : SEQ_TYPE;
      Res_Type   : TREE;
      Handled    : out Boolean) return Expr_Val;

end Cg_Expr.RTL;
