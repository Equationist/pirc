-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with Interfaces; use Interfaces;
with USERPK; use USERPK;
with DIANAMAN; use DIANAMAN;
with Cg_State; use Cg_State;

package Cg_Stmt is

   -- Emit a complete DIANA compilation node (all compilation units, or only Only_CU when > 0)
   Elab_Cycle_Error : exception;

   procedure Emit_Compilation (Ctx : in out Context; Comp_Node : TREE; Only_CU : Natural := 0);

private

   type Cap_Pair is record
      Var_Key  : Unsigned_32 := 0;
      Inst_Key : Unsigned_32 := 0;
   end record;

   package Cap_Vecs is new Ada.Containers.Vectors
     (Index_Type   => Natural,
      Element_Type => Cap_Pair);

   -- Internal cross-domain entry points shared with Cg_Stmt child packages
   procedure Pre_Register_Subp
     (Ctx         : in out Context;
      Subp_Id     : TREE;
      Spec_Node   : TREE;
      Parent_Subp : Unsigned_32;
      Is_Subunit  : Boolean := False);

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

   procedure Emit_Decl_Items
     (Ctx        : in out Context;
      Item_S     : TREE;
      Is_Global  : Boolean;
      Export_All : Boolean;
      Has_Dyn    : out Boolean;
      Only_Item  : TREE := CONST_VOID);

   procedure Emit_Block_Body
     (Ctx         : in out Context;
      Block_Node  : TREE;
      Is_Subp     : Boolean);

   procedure Emit_Stm_List (Ctx : in out Context; Stm_S : TREE);
   procedure Emit_Stm (Ctx : in out Context; Stm : TREE);

   procedure Emit_Exception_Handlers
     (Ctx        : in out Context;
      Alt_S      : TREE;
      Hdl_Label  : String;
      After_Lbl  : String);

   function Type_Contains_Task (Ctx : in out Context; Raw_T : TREE; Depth : Natural := 0) return Boolean;
   function Item_List_Has_Tasks (Ctx : in out Context; Item_S : TREE) return Boolean;
   procedure Scan_Label_Cleanup_Depths (Ctx : in out Context; Node : TREE; Depth : Natural);

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

   function Is_Named_No_Others_Agg (AN : TREE; Rem_D : Natural) return Boolean;

end Cg_Stmt;
