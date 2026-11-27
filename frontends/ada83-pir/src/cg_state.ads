-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Hashed_Maps;
with Ada.Containers.Hashed_Sets;
with Ada.Containers.Vectors;
with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces; use Interfaces;
with USERPK; use USERPK;
with DIANA; use DIANA;
with Pir_Emit;

package Cg_State is

   function Hash_U32 (K : Unsigned_32) return Ada.Containers.Hash_Type;
   function Hash_U64 (K : Unsigned_64) return Ada.Containers.Hash_Type;

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

   Max_Array_Dims : constant Natural := 8;
   Arr_Desc_Size  : constant Natural := 8 + 2 * Max_Array_Dims * 8; -- 136 bytes

   type Dim_Array is array (1 .. Max_Array_Dims) of Dim_Bound;

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
      Field_Idx           : Natural := 0;
      In_Union            : Boolean := False;
      Union_Idx           : Natural := 0;
      Var_Idx             : Natural := 0;
      Var_Type            : Unbounded_String;
      Var_Field           : Natural := 0;
      Byte_Offset         : Natural := 0;
      Field_Size          : Natural := 0;
      Has_Explicit_Offset : Boolean := False;
      Bit_Offset          : Natural := 0;
      Bit_Size            : Natural := 0;
   end record;

   package Field_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Field_Entry,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   -- Variable / object binding
   type Var_Kind is
     (VK_Global,
      VK_Global_Ptr,
      VK_Local_Alloca,
      VK_Local_Ptr,
      VK_Param_Val,
      VK_Param_Ptr,
      VK_Uncon_Array_Param,
      VK_Dyn_Array,
      VK_Const_Int,
      VK_Const_Float);

   type Str_Array_4 is array (1 .. Max_Array_Dims) of Unbounded_String;

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
      Has_Desc       : Boolean := False;
      Desc_Global    : Unbounded_String;
      -- For compile-time scalar constants:
      Const_I64      : Integer_64 := 0;
      Const_F64      : IEEE_Float_64 := 0.0;
      -- For read-only loop parameters with statically known bounds:
      Has_Static_Rng  : Boolean := False;
      Rng_Lo          : Integer_64 := 0;
      Rng_Hi          : Integer_64 := 0;
      -- For OUT / IN OUT unconstrained record parameters with default discriminants (LRM 3.7.2/3.7.4):
      Constrained_Reg : Unbounded_String;
      Con_Captured_Slot : Integer := -1;
      Con_Global      : Unbounded_String;
      -- For pragma SHARED propagation through renamings (LRM 9.11):
      Is_Shared       : Boolean := False;
      -- For bitfield component object renamings (LRM 8.5, 13.4):
      Is_Bitfield     : Boolean := False;
      BF_FE           : Field_Entry;
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
      Is_External    : Boolean := False;
      Is_Emitted     : Boolean := False;
      Is_C_Interface : Boolean := False;
      Elab_Flag      : Unbounded_String;
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
      Label_Key     : Unsigned_32 := 0; -- 0 if unnamed
      Exit_Label    : Unbounded_String;
      Cleanup_Depth : Natural := 0;
   end record;

   package Loop_Vecs is new Ada.Containers.Vectors (Natural, Loop_Entry);

   -- Unified LIFO scope cleanup stack (masters, secondary-stack marks, dynamic stack saves)
   type Cleanup_Kind is (CK_Master, CK_SS_Mark, CK_SP_Mark);

   type Cleanup_Entry is record
      Kind : Cleanup_Kind := CK_Master;
      Reg  : Unbounded_String;
   end record;

   package Cleanup_Vecs is new Ada.Containers.Vectors (Natural, Cleanup_Entry);

   -- Exception handler stack
   type Handler_Entry is record
      Handler_Label : Unbounded_String;
      Current_Exn   : Unbounded_String; -- active caught exn reg inside handler body
      In_Handler    : Boolean := False;
      SS_Mark       : Unbounded_String; -- optional SS mark to release upon entering handler
      SP_Mark       : Unbounded_String; -- optional SP mark to restore upon entering handler
   end record;

   package Handler_Vecs is new Ada.Containers.Vectors (Natural, Handler_Entry);

   -- Active secondary-stack marks in the current subprogram
   package Str_Vecs is new Ada.Containers.Vectors (Natural, Unbounded_String);

   -- Captured variable list per subprogram
   package U32_Vecs is new Ada.Containers.Vectors (Natural, Unsigned_32);

   package U32_Sets is new Ada.Containers.Hashed_Sets
     (Element_Type        => Unsigned_32,
      Hash                => Hash_U32,
      Equivalent_Elements => "=");

   package U32_Str_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Unbounded_String,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   package U64_Str_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_64,
      Element_Type    => Unbounded_String,
      Hash            => Hash_U64,
      Equivalent_Keys => "=");

   package Str_Map_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => U32_Str_Maps.Map,
      Hash            => Hash_U32,
      Equivalent_Keys => "=",
      "="             => U32_Str_Maps."=");

   type Access_Master_Info is record
      Local_Ptr  : Unbounded_String;
      Global_Sym : Unbounded_String;
      Owner_Subp : Unsigned_32 := 0;
      Cap_Slot   : Integer := 0;
   end record;

   package Access_Master_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Access_Master_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   -- Scalar out / in-out copy-out entries (Ada 83 LRM 6.2(7) by-copy semantics)
   type Copy_Out_Entry is record
      Ptr_Reg  : Unbounded_String;
      Loc_Reg  : Unbounded_String;
      Pir_Type : Unbounded_String;
      Align    : Natural := 4;
      Ada_Type : TREE := (0, 0, 0);
   end record;

   package Copy_Out_Vecs is new Ada.Containers.Vectors (Natural, Copy_Out_Entry);

   -- Task type & entry descriptors (Ada 83 Chapter 9)
   type Entry_Info is record
      Task_Key      : Unsigned_32 := 0;
      Base_Idx      : Natural := 0;
      Is_Family     : Boolean := False;
      Fam_Is_Static : Boolean := True;
      Fam_Lo        : Integer_64 := 0;
      Fam_Hi        : Integer_64 := 0;
      Fam_DR        : TREE := (0, 0, 0);
      Fam_Base_Sym  : Unbounded_String;
      Fam_Lo_Sym    : Unbounded_String;
      Fam_Hi_Sym    : Unbounded_String;
      Fam_Exported  : Boolean := False;
      Entry_Header  : TREE := (0, 0, 0);
      Pkt_Type      : Unbounded_String;
      Has_Params    : Boolean := False;
      Has_Address   : Boolean := False;
      Addr_Exp      : TREE := (0, 0, 0);
   end record;

   type Renamed_Entry_Info is record
      Task_Sym      : Unbounded_String;
      Idx_Sym       : Unbounded_String;
      Entry_Id      : TREE := (0, 0, 0);
      Is_Local      : Boolean := False;
      Owner_Subp    : Unsigned_32 := 0;
      Fallback_TSym : Unbounded_String := Null_Unbounded_String;
      Fallback_ISym : Unbounded_String := Null_Unbounded_String;
   end record;

   package Renamed_Entry_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Renamed_Entry_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   package I64_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Integer_64,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   package Entry_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Entry_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   type Task_Info is record
      Body_Fn          : Unbounded_String;
      Num_Entries      : Natural := 1;
      Has_Dyn_Entries  : Boolean := False;
      Dyn_Entries_Sym  : Unbounded_String;
      Dyn_Entries_Exp  : Boolean := False;
      Parent_Subp      : Unsigned_32 := 0;
      Needs_SL         : Boolean := False;
      Is_Emitted       : Boolean := False;
      Elab_Flag        : Unbounded_String;
      Elab_Is_Local    : Boolean := False;
      Elab_Owner       : Unsigned_32 := 0;
      Elab_Cap_Slot    : Natural := 0;
      Elab_Fallback    : Unbounded_String;
      Inst_Tag         : Unbounded_String;
      Has_Priority     : Boolean := False;
      Priority         : Integer := 0;
      Storage_Size     : Integer_64 := 0;
      Storage_Size_Exp : TREE := (0, 0, 0);
   end record;

   package Task_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Task_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   package Tree_Map_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Tree_Maps.Map,
      Hash            => Hash_U32,
      Equivalent_Keys => "=",
      "="             => Tree_Maps."=");

   package Var_Map_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Var_Maps.Map,
      Hash            => Hash_U32,
      Equivalent_Keys => "=",
      "="             => Var_Maps."=");

   package Subp_Map_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Subp_Maps.Map,
      Hash            => Hash_U32,
      Equivalent_Keys => "=",
      "="             => Subp_Maps."=");

   package Task_Map_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Task_Maps.Map,
      Hash            => Hash_U32,
      Equivalent_Keys => "=",
      "="             => Task_Maps."=");

   package U64_Nat_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_64,
      Element_Type    => Natural,
      Hash            => Hash_U64,
      Equivalent_Keys => "=");

   -- Pre-evaluated subtype bounds / discriminants / component constraint expressions (LRM 3.2.1, 3.7.2)
   type Saved_Range_Info is record
      Lo_Ptr     : Unbounded_String;
      Hi_Ptr     : Unbounded_String;
      Pir_Type   : Unbounded_String;
      Align      : Natural := 4;
      Is_Global  : Boolean := False;
      Owner_Subp : Unsigned_32 := 0;
   end record;

   package Saved_Range_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Saved_Range_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   type Saved_Disc_Ptrs  is array (1 .. 8) of Unbounded_String;
   type Saved_Disc_Types is array (1 .. 8) of Unbounded_String;
   type Saved_Disc_Aligns is array (1 .. 8) of Natural;

   type Saved_Disc_Info is record
      Count      : Natural := 0;
      Ptrs       : Saved_Disc_Ptrs;
      Pir_Types  : Saved_Disc_Types;
      Aligns     : Saved_Disc_Aligns := (others => 4);
      Is_Global  : Boolean := False;
      Owner_Subp : Unsigned_32 := 0;
   end record;

   package Saved_Disc_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Saved_Disc_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   type Saved_Expr_Info is record
      Ptr_Reg    : Unbounded_String;
      Pir_Type   : Unbounded_String;
      Ada_Type   : TREE := (0, 0, 0);
      Align      : Natural := 4;
      Is_Global  : Boolean := False;
      Owner_Subp : Unsigned_32 := 0;
      Cap_Slot   : Natural := 0;
   end record;

   package Saved_Expr_Maps is new Ada.Containers.Hashed_Maps
     (Key_Type        => Unsigned_32,
      Element_Type    => Saved_Expr_Info,
      Hash            => Hash_U32,
      Equivalent_Keys => "=");

   type Accept_Frame is record
      End_Label     : Unbounded_String;
      Cleanup_Depth : Natural := 0;
   end record;

   package Accept_Vecs is new Ada.Containers.Vectors (Natural, Accept_Frame);

   type Context is limited record
      E             : Pir_Emit.Emitter;
      Enable_Checks : Boolean := True;
      Emit_Main     : Boolean := True;
      Lib_Dir       : Unbounded_String := To_Unbounded_String ("ADA__LIB");

      Types            : Type_Maps.Map;
      In_Progress_Recs : U32_Sets.Set; -- cycle guard for recursive access-to-record types
      Fields           : Field_Maps.Map;
      Vars             : Var_Maps.Map;
      Subps            : Subp_Maps.Map;
      Tasks            : Task_Maps.Map;
      Entries          : Entry_Maps.Map;
      Generic_Decls    : Tree_Maps.Map;
      Generic_Body     : Tree_Maps.Map;
      Gen_Subunits     : Tree_Maps.Map; -- separate subunit bodies belonging to generic templates (c94008d, ca3011a)
      Generic_Env      : Tree_Map_Maps.Map; -- captured outer Generic_Subst for nested generics
      Gen_Env_Exc      : Str_Map_Maps.Map; -- captured outer Gen_Local_Exc_Tags for nested generics
      Gen_Env_Vars     : Var_Map_Maps.Map;  -- captured outer generic body Vars for nested generics
      Gen_Env_Subps    : Subp_Map_Maps.Map; -- captured outer generic body Subps for nested generics
      Gen_Env_Tasks    : Task_Map_Maps.Map; -- captured outer generic body Tasks for nested generics
      Gen_Cap_Slots    : U64_Nat_Maps.Map;  -- (Inst_Key << 32 | Var_Key) -> parent frame slot for multi-inst local generics
      Gen_Body_Nested_Gens : U32_Vecs.Vector; -- nested generic keys registered during active outer generic body
      Gen_Elab_Flags   : U32_Str_Maps.Map; -- generic key -> @ada_gelab_... global
      Generic_Subst    : Tree_Maps.Map; -- active formal -> actual during generic emission
      Subp_Renames     : Tree_Maps.Map; -- SID -> DN_RENAMES_UNIT (when instant.a leaves SM_UNIT_DESC void)
      Gen_Inst_Tag     : Unbounded_String; -- per-instantiation disambiguator for local subps
      In_Gen_Body      : Boolean := False; -- True while emitting a generic body (incl. nested packages inside it)
      In_Subunit       : Boolean := False; -- True when emitting a DN_SUBUNIT compilation unit
      In_Lib_Pkg_Body  : Boolean := False; -- True when emitting a non-support library package body
      Has_Stubs        : Boolean := False; -- True when emitting a unit that contains DN_STUB declarations
      Exc_Tags         : U32_Str_Maps.Map; -- exception key -> @__ada_exc_... symbol name

      -- Current subprogram / task state
      Cur_Subp_Key      : Unsigned_32 := 0;
      Cur_Subp_Node     : TREE := (0, 0, 0);
      Cur_Ret_Type      : TREE := (0, 0, 0);
      Cur_Ret_Comp      : Boolean := False;
      Cur_Ret_Uncon     : Boolean := False;
      Cur_Ret_Ptr       : Unbounded_String;
      Cur_Frame_Reg     : Unbounded_String;
      Cur_SL_Reg        : Unbounded_String;
      Cur_Master_Reg    : Unbounded_String;
      Cur_Task_Reg                : Unbounded_String;
      Cur_Task_Exit_Lbl           : Unbounded_String;
      Cur_Task_Base_Cleanup_Depth : Natural := 0;
      Cur_Is_Func                 : Boolean := False;
      Uplevel_Cache     : U64_Str_Maps.Map; -- (Owner_Subp << 32 | Slot) -> entry-block uplevel var addr reg
      Pkg_Task_Groups   : I64_Maps.Map;  -- local package task group IDs (PID key -> group_id)
      Cur_Task_Group_Id : Integer_64 := 0;
      Next_Task_Group_Id : Integer_64 := 1;
      Renamed_Entries   : Renamed_Entry_Maps.Map; -- SID key -> pre-evaluated task/idx globals
      Access_Masters    : Access_Master_Maps.Map; -- access base type key -> declaring master binding
      Saved_Type_Ranges : Saved_Range_Maps.Map; -- scalar subtype key -> pre-evaluated Lo/Hi allocas/globals
      Saved_Var_Ranges  : Saved_Range_Maps.Map; -- object ID key -> per-object frozen Lo/Hi (multi-object decls)
      Saved_Type_Discs  : Saved_Disc_Maps.Map;  -- constrained record key -> pre-evaluated disc allocas/globals
      Saved_Expr_Vals   : Saved_Expr_Maps.Map;  -- record comp constraint expr key -> pre-evaluated alloca/global
      Gen_Local_Exc_Tags : U32_Str_Maps.Map;    -- generic-local exception key -> tagged @__ada_exc_... sym

      Loops                : Loop_Vecs.Vector;
      Handlers             : Handler_Vecs.Vector;
      Copy_Outs            : Copy_Out_Vecs.Vector;
      Cleanups             : Cleanup_Vecs.Vector;
      Accept_Frames        : Accept_Vecs.Vector;
      Label_Cleanup_Depths : I64_Maps.Map;
   end record;

   procedure Init_Context
     (Ctx           : in out Context;
      Target_Triple : String;
      Enable_Checks : Boolean;
      Emit_Main     : Boolean);

   -- Scoped temporary Var_Maps binding guard (restores shadowed outer bindings in LIFO order)
   type Saved_Var_Entry is record
      Key      : Unsigned_32 := 0;
      Had_Prev : Boolean := False;
      Prev_Var : Var_Info;
   end record;

   package Temp_Var_Vecs is new Ada.Containers.Vectors (Natural, Saved_Var_Entry);
   subtype Temp_Var_Scope is Temp_Var_Vecs.Vector;

   -- Snapshot of all per-subprogram / per-task emission state on Context
   type Subp_State_Snapshot is record
      Cur_Subp_Key                : Unsigned_32 := 0;
      Cur_Subp_Node               : TREE := (0, 0, 0);
      Cur_Ret_Type                : TREE := (0, 0, 0);
      Cur_Ret_Comp                : Boolean := False;
      Cur_Ret_Uncon               : Boolean := False;
      Cur_Ret_Ptr                 : Unbounded_String;
      Cur_Frame_Reg               : Unbounded_String;
      Cur_SL_Reg                  : Unbounded_String;
      Cur_Master_Reg              : Unbounded_String;
      Cur_Task_Reg                : Unbounded_String;
      Cur_Task_Exit_Lbl           : Unbounded_String;
      Cur_Task_Base_Cleanup_Depth : Natural := 0;
      Cur_Is_Func                 : Boolean := False;
      Loops                       : Loop_Vecs.Vector;
      Handlers                    : Handler_Vecs.Vector;
      Copy_Outs                   : Copy_Out_Vecs.Vector;
      Cleanups                    : Cleanup_Vecs.Vector;
      Accept_Frames               : Accept_Vecs.Vector;
      Label_Cleanup_Depths        : I64_Maps.Map;
      Uplevel_Cache               : U64_Str_Maps.Map;
   end record;

   -- Apply active generic substitution (if any) to a DIANA type or definition node.
   -- Type resolution hierarchy:
   --   Subst_Resolve_Type : apply generic map + private/incomplete views (keeps subtype constraints)
   --   Base_Of_Type       : strip subtype constraints (SM_BASE_TYPE) ONLY; stops at derived types!
   --   Root_Base_Type / Subst_Base_Type : strip subtypes AND follow SM_DERIVED on enums/privates to root
   function Subst_Tree (Ctx : Context; T : TREE) return TREE;
   function Subst_Resolve_Type (Ctx : Context; T : TREE) return TREE;
   function Base_Of_Type (Ctx : Context; T : TREE) return TREE;
   function Root_Base_Type (Ctx : Context; T : TREE) return TREE;
   function Subst_Base_Type (Ctx : Context; T : TREE) return TREE;

   -- Scoped temporary variable/discriminant binding helpers
   procedure Bind_Temp_Var
     (Ctx   : in out Context;
      Scope : in out Temp_Var_Scope;
      Key   : Unsigned_32;
      Val   : Var_Info);
   procedure Restore_Temp_Vars
     (Ctx   : in out Context;
      Scope : in out Temp_Var_Scope);

   -- Save / reset / restore per-subprogram & per-task Context state cleanly
   function Save_Subp_State (Ctx : Context) return Subp_State_Snapshot;
   procedure Reset_Subp_State (Ctx : in out Context);
   procedure Restore_Subp_State (Ctx : in out Context; Snap : Subp_State_Snapshot);

   -- Emit element pointer for array of ETI (handles array-of-arrays via ptr_add byte stride!)
   function Emit_Elem_Ptr
     (Ctx      : in out Context;
      ETI      : Type_Info;
      Base_Ptr : String;
      Idx_64   : String) return String;

   -- Resolve and materialize PIR type info for any DIANA type spec
   function Get_Type_Info (Ctx : in out Context; Raw_Type : TREE) return Type_Info;

   -- Check whether a record/private type has discriminants with defaults, or whether a param needs a hidden 'CONSTRAINED i8 arg
   function Record_Has_Default_Discs (Ctx : Context; Raw_T : TREE) return Boolean;
   function Needs_Constrained_Param (Ctx : Context; Raw_T : TREE; Mode : NODE_NAME) return Boolean;

   -- Get exception global tag symbol name (e.g. "@__ada_exc_constraint_error")
   function Get_Exc_Tag (Ctx : in out Context; Exc_Id : TREE) return String;
   function Constraint_Error_Tag (Ctx : in out Context) return String;
   function Program_Error_Tag (Ctx : in out Context) return String;
   function Storage_Error_Tag (Ctx : in out Context) return String;

   -- Register a task specification and its entries / entry param packet types
   procedure Register_Task_Spec
     (Ctx          : in out Context;
      Task_Spec    : TREE;
      Parent_Subp  : Unsigned_32;
      Force_Rebind : Boolean := False);

   -- Load enclosing task's %__self via static-link chain (for nested subprograms/tasks)
   function Emit_Enclosing_Task_Self_By_Key
     (Ctx       : in out Context;
      Target_TK : Unsigned_32) return String;

   -- Emit runtime externs are declared
   procedure Ensure_Runtime_Externs (Ctx : in out Context);

   -- Check whether a subprogram has `pragma INTERFACE (C, ...)`
   function Is_C_Interfaced (Subp_Id : TREE) return Boolean;

   -- Compute deterministic mangled PIR symbol name for a subprogram
   function Mangle_Subp_Name
     (Ctx        : in out Context;
      Subp_Id    : TREE;
      Spec_Node  : TREE;
      Is_Subunit : Boolean := False) return String;

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
     (Ctx            : in out Context;
      Spec_Node      : TREE;
      Needs_SL       : Boolean;
      Ret_Comp       : Boolean;
      Ret_Type       : TREE;
      With_Names     : Boolean;
      Is_C_Interface : Boolean := False) return String;

   -- Safe alignment for a record component (align 1 when placed via record rep clause, P3-6.4)
   function Explicit_Field_Align (FE : Field_Entry; Nat_Align : Natural) return Natural;

   -- Helper to get uplevel variable address via static link chain
   function Emit_Var_Addr (Ctx : in out Context; V : Var_Info) return String;
   function Emit_Captured_Slot_Ptr (Ctx : in out Context; Owner_Subp : Unsigned_32; Slot : Integer) return String;

   -- Get (and register) PIR descriptor struct type for N-dim returned unconstrained array
   function Get_Arr_Desc_Type (Ctx : in out Context; NDims : Natural) return String;

   -- Unified scope cleanup stack operations (masters, secondary stack, stacksave)
   function Cleanup_Depth (Ctx : Context) return Natural;
   procedure Push_Cleanup (Ctx : in out Context; Kind : Cleanup_Kind; Reg : String);
   procedure Pop_Cleanup (Ctx : in out Context; Expected : Cleanup_Kind);
   function Latest_Cleanup_Reg (Ctx : Context; Kind : Cleanup_Kind) return Unbounded_String;
   function Needs_Unwind_To
     (Ctx           : Context;
      Target_Depth  : Natural;
      Leave_Masters : Boolean := True;
      Release_SS    : Boolean := True;
      Restore_SP    : Boolean := True) return Boolean;
   procedure Emit_Unwind_To
     (Ctx           : in out Context;
      Target_Depth  : Natural;
      Leave_Masters : Boolean := True;
      Release_SS    : Boolean := True;
      Restore_SP    : Boolean := True);

   -- Storage byte size / alignment of an element stored inside an array or record
   -- (dynamic constrained arrays are stored inline as Arr_Desc_Size-byte @__ada_arr_desc structs).
   function Elem_Storage_Size (ETI : Type_Info) return Natural;
   function Elem_Storage_Align (ETI : Type_Info) return Natural;

end Cg_State;
