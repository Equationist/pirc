-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Expr.Attr is

   function Emit_Attribute
     (Ctx         : in out Context;
      Node        : TREE;
      Target_Type : TREE) return Expr_Val;

end Cg_Expr.Attr;
