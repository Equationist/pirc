-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Hashed_Maps;
with Ada.Containers.Vectors;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces; use Interfaces;
with USERPK; use USERPK;
with Pir_Emit;

package Cg_State is

   function Hash_U32 (K : Unsigned_32) return Ada.Containers.Hash_Type;

   -- Type descriptor cached per DIANA type spec
   type Type_Kind is
     (TK_Scalar,
      TK_Record,
      TK_Constrained_Array,
      TK_Unconstrained_Array,
      TK_Void);

   type Dim_Bound is record
      Is_Static : Boolean := False;
      First     : Integer_64 := 0;
      Last      : Integer_64 := 0;
      Idx_Type  : TREE := (0, 0, 0);
   end record;

   type Dim_Array is array (1 .. 4) of Dim_Bound;

   type Type_Info is record
      Kind        : Type_Kind := TK_Void;
      Pir_Type    : Unbounded_String;
      Byte_Size   : Natural := 0;
      Align       : Natural := 1;
      NDims       : Natural := 0;
      Dims        : Dim_Array;
      Elem_Type   : TREE := (0, 0, 0);
      Is_Dynamic  : Boolean := False;
   end record;

   package Type_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Type_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   -- Field index map: (Record_Type_Key, Comp_Id_Key) -> Field_Index (0-based)
   type Field_Entry is record
      Field_Idx : Natural := 0;
      In_Union  : Boolean := False;
      Union_Idx : Natural := 0;
      Var_Idx   : Natural := 0;
      Var_Type  : Unbounded_String;
      Var_Field : Natural := 0;
   end record;

   package Field_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Field_Entry,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   -- Variable / object binding
   type Var_Kind is
     (VK_Global,
      VK_Local_Alloca,
      VK_Param_Val,
      VK_Param_Ptr,
      VK_Uncon_Array_Param,
      VK_Dyn_Array,
      VK_Const_Int,
      VK_Const_Float);

   type Str_Array_4 is array (1 .. 4) of Unbounded_String;

   type Var_Info is record
      Kind           : Var_Kind := VK_Local_Alloca;
      Pir_Name       : Unbounded_String;
      Pir_Type       : Unbounded_String;
      Ada_Type       : TREE := (0, 0, 0);
      Align          : Natural := 4;
      Owner_Subp     : Unsigned_32 := 0;
      Captured_Slot  : Integer := -1; -- >= 1 if stored in Owner_Subp's static-link frame
      -- For unconstrained / dynamic arrays:
      NDims          : Natural := 0;
      First_Regs     : Str_Array_4;
      Last_Regs      : Str_Array_4;
      -- For compile-time scalar constants:
      Const_I64      : Integer_64 := 0;
      Const_F64      : IEEE_Float_64 := 0.0;
      -- For read-only loop parameters with statically known bounds:
      Has_Static_Rng : Boolean := False;
      Rng_Lo         : Integer_64 := 0;
      Rng_Hi         : Integer_64 := 0;
   end record;

   package Var_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Var_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   -- Subprogram binding
   type Subp_Info is record
      Pir_Name      : Unbounded_String;
      Spec_Node     : TREE := (0, 0, 0);
      Needs_SL      : Boolean := False;
      Has_Frame     : Boolean := False;
      Frame_Type    : Unbounded_String;
      Parent_Subp   : Unsigned_32 := 0;
      Is_Function   : Boolean := False;
      Ret_Composite : Boolean := False;
      Ret_Uncon_Arr : Boolean := False;
      Ret_Type      : TREE := (0, 0, 0);
      Is_External   : Boolean := False;
      Is_Emitted    : Boolean := False;
   end record;

   package Subp_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Subp_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   -- Generic body / decl registration and active substitution map
   package Tree_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => TREE,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   -- Loop exit label stack
   type Loop_Entry is record
      Label_Key  : Unsigned_32 := 0; -- 0 if unnamed
      Exit_Label : Unbounded_String;
      SS_Depth   : Natural := 0;
   end record;

   package Loop_Vecs is new Ada.Containers.Vectors (Natural, Loop_Entry);

   -- Exception handler stack
   type Handler_Entry is record
      Handler_Label : Unbounded_String;
      Current_Exn   : Unbounded_String; -- active caught exn reg inside handler body
      In_Handler    : Boolean := False;
      SS_Mark       : Unbounded_String; -- optional SS mark to release upon entering handler
   end record;

   package Handler_Vecs is new Ada.Containers.Vectors (Natural, Handler_Entry);

   -- Active secondary-stack marks in the current subprogram
   package Str_Vecs is new Ada.Containers.Vectors (Natural, Unbounded_String);

   -- Pending generic instantiations (for instantiations appearing before generic body)
   type Pending_Inst is record
      Inst_Decl : TREE := (0, 0, 0);
      Owner_Subp : Unsigned_32 := 0;
   end record;

   package Pending_Inst_Vecs is new Ada.Containers.Vectors (Natural, Pending_Inst);

   -- Captured variable list per subprogram
   package U32_Vecs is new Ada.Containers.Vectors (Natural, Unsigned_32);

   -- Scalar out / in-out copy-out entries (Ada 83 LRM 6.2(7) by-copy semantics)
   type Copy_Out_Entry is record
      Ptr_Reg  : Unbounded_String;
      Loc_Reg  : Unbounded_String;
      Pir_Type : Unbounded_String;
      Align    : Natural := 4;
      Ada_Type : TREE := (0, 0, 0);
   end record;

   package Copy_Out_Vecs is new Ada.Containers.Vectors (Natural, Copy_Out_Entry);

   type Context is limited record
      E             : Pir_Emit.Emitter;
      Enable_Checks : Boolean := True;
      Emit_Main     : Boolean := True;
      Main_Override : Unbounded_String;

      Types         : Type_Maps.Map;
      Fields        : Field_Maps.Map;
      Vars          : Var_Maps.Map;
      Subps         : Subp_Maps.Map;
      Generic_Decls : Tree_Maps.Map;
      Generic_Body  : Tree_Maps.Map;
      Generic_Subst : Tree_Maps.Map; -- active formal -> actual during generic emission
      Exc_Tags      : Type_Maps.Map; -- reuse U32 -> Pir_Type holds tag symbol name

      -- Current subprogram state
      Cur_Subp_Key  : Unsigned_32 := 0;
      Cur_Subp_Node : TREE := (0, 0, 0);
      Cur_Ret_Type  : TREE := (0, 0, 0);
      Cur_Ret_Comp  : Boolean := False;
      Cur_Ret_Uncon : Boolean := False;
      Cur_Ret_Ptr   : Unbounded_String;
      Cur_Frame_Reg : Unbounded_String;
      Cur_SL_Reg    : Unbounded_String;
      Cur_Is_Func   : Boolean := False;
      Uplevel_Cache : Type_Maps.Map; -- cached entry-block uplevel var address regs per subprogram

      Loops         : Loop_Vecs.Vector;
      Handlers      : Handler_Vecs.Vector;
      Copy_Outs     : Copy_Out_Vecs.Vector;
      SS_Marks      : Str_Vecs.Vector;
      Pending_Insts : Pending_Inst_Vecs.Vector;

      -- Withed user packages to elaborate in main
      Elab_Funcs    : U32_Vecs.Vector;
      Elab_Names    : Unbounded_String; -- pipe-delimited list of elab func symbols
   end record;

   procedure Init_Context
     (Ctx           : in out Context;
      Target_Triple : String;
      Enable_Checks : Boolean;
      Emit_Main     : Boolean);

   -- Apply active generic substitution (if any) to a DIANA type or definition node
   function Subst_Tree (Ctx : Context; T : TREE) return TREE;

   -- Resolve and materialize PIR type info for any DIANA type spec
   function Get_Type_Info (Ctx : in out Context; Raw_Type : TREE) return Type_Info;

   -- Get exception global tag symbol name (e.g. "@__ada_exc_constraint_error")
   function Get_Exc_Tag (Ctx : in out Context; Exc_Id : TREE) return String;
   function Constraint_Error_Tag (Ctx : in out Context) return String;
   function Program_Error_Tag (Ctx : in out Context) return String;
   function Storage_Error_Tag (Ctx : in out Context) return String;

   -- Ensure runtime externs are declared
   procedure Ensure_Runtime_Externs (Ctx : in out Context);

   -- Compute deterministic mangled PIR symbol name for a subprogram
   function Mangle_Subp_Name (Ctx : in out Context; Subp_Id : TREE; Spec_Node : TREE) return String;

   -- Emit a raise of an exception tag (jumps to local handler if inside one, else resume)
   procedure Emit_Raise_Tag (Ctx : in out Context; Tag_Sym : String);
   procedure Emit_Raise_Reg (Ctx : in out Context; Exn_Reg : String);

   -- Check if currently inside a handled region (so calls should use invoke)
   function Has_Active_Handler (Ctx : Context) return Boolean;
   function Active_Handler_Label (Ctx : Context) return String;

   -- Emit a call (or invoke if inside a handler)
   procedure Emit_Call_Or_Invoke
     (Ctx       : in out Context;
      Func_Sym  : String;
      Args_Str  : String;
      Ret_Type  : String;  -- "" for void
      Res_Reg   : out Unbounded_String;
      Nounwind  : Boolean := False);

   -- Ensure an external subprogram declaration is emitted
   procedure Ensure_Extern_Subp (Ctx : in out Context; Subp_Id : TREE; Info : Subp_Info);

   -- Build PIR parameter signature string for a subprogram
   function Build_Param_Sig
     (Ctx        : in out Context;
      Spec_Node  : TREE;
      Needs_SL   : Boolean;
      Ret_Comp   : Boolean;
      Ret_Type   : TREE;
      With_Names : Boolean) return String;

   -- Helper to get uplevel variable address via static link chain
   function Emit_Var_Addr (Ctx : in out Context; V : Var_Info) return String;

   -- Get (and register) PIR descriptor struct type for N-dim returned unconstrained array
   function Get_Arr_Desc_Type (Ctx : in out Context; NDims : Natural) return String;

end Cg_State;
