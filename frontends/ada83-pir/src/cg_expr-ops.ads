-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Expr.Ops is

   function Emit_Builtin_Op
     (Ctx        : in out Context;
      Op_Id      : TREE;
      Param_List : SEQ_TYPE;
      Res_Type   : TREE) return Expr_Val;

end Cg_Expr.Ops;
