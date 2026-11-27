-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Stmt.Generics is

   function Find_Gen_Subunit_Body (Ctx : Context; Item : TREE) return TREE;
   function Is_Gen_Subunit_Stub (Ctx : Context; Item : TREE) return Boolean;
   function Is_RTL_Generic (Gen_Id : TREE) return Boolean;
   function Tree_Uses_SS (Ctx : in out Context; Node : TREE) return Boolean;
   function Tree_Has_Return (Node : TREE) return Boolean;
   procedure Map_Decl_List_Ids (Ctx : in out Context; Gen_DS : TREE; Inst_DS : TREE);

   procedure Scan_Tree_For_Captures
     (Ctx           : in out Context;
      Node          : TREE;
      Current_Subp  : Unsigned_32;
      Owner_Map     : Var_Maps.Map;
      Parent_Map    : in out Subp_Maps.Map;
      Captured_Vars : in out Cap_Vecs.Vector;
      Nested_Subps  : in out U32_Vecs.Vector;
      Target_Owner  : Unsigned_32;
      Type_Depth    : Natural := 0;
      Inst_Key      : Unsigned_32 := 0;
      Inst_Gen_Id   : TREE := CONST_VOID);

   procedure Emit_Generic_Instantiation
     (Ctx        : in out Context;
      Inst_Decl  : TREE;
      Export_All : Boolean);

end Cg_Stmt.Generics;
