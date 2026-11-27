-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Stmt.Decls is

   function Is_Named_No_Others_Agg (AN : TREE; Rem_D : Natural) return Boolean;

   procedure Pre_Register_Subp
     (Ctx         : in out Context;
      Subp_Id     : TREE;
      Spec_Node   : TREE;
      Parent_Subp : Unsigned_32;
      Is_Subunit  : Boolean := False);

   procedure Emit_Decl_Items
     (Ctx        : in out Context;
      Item_S     : TREE;
      Is_Global  : Boolean;
      Export_All : Boolean;
      Has_Dyn    : out Boolean;
      Only_Item  : TREE := CONST_VOID);

end Cg_Stmt.Decls;
