-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces; use Interfaces;
with USERPK; use USERPK;
with DIANAMAN; use DIANAMAN;
with Cg_State; use Cg_State;

package Cg_Expr is

   type Expr_Kind is
     (EK_Scalar,
      EK_Addr,           -- Pointer to composite (record / constrained array)
      EK_Slice,          -- Unconstrained array / slice / string: (Ptr, First(1..4), Last(1..4))
      EK_Void);

   type Expr_Val is record
      Kind       : Expr_Kind := EK_Void;
      Reg        : Unbounded_String;
      Pir_Type   : Unbounded_String;
      Ada_Type   : TREE := (0, 0, 0);
      Align      : Natural := 4;
      NDims      : Natural := 0;
      First_Regs : Str_Array_4;
      Last_Regs  : Str_Array_4;
   end record;

   -- Evaluate an expression (produces a scalar value, composite pointer, or slice descriptor)
   function Emit_Expr (Ctx : in out Context; Node : TREE; Target_Type : TREE := (0, 0, 0)) return Expr_Val;

   -- Evaluate an lvalue (returns address pointer or slice descriptor for assignment/out param)
   function Emit_LValue (Ctx : in out Context; Node : TREE) return Expr_Val;

   -- Emit scalar cast/widening/narrowing between PIR types (int, float, ptr)
   function Int_Bits_Of_Pir (Pir_Ty : String) return Natural;

   -- Emit a verifier-safe integer constant register for Pir_Ty (i8/i16/i32/i64)
   function Emit_Int_Const
     (Ctx    : in out Context;
      Val    : Integer_64;
      Pir_Ty : String;
      Prefix : String := "c") return String;

   function Emit_Int_Convert
     (Ctx         : in out Context;
      Val_Reg     : String;
      From_Ty     : String;
      To_Ty       : String;
      Is_Unsigned : Boolean := False) return String;

   function Emit_Scalar_Coerce
     (Ctx         : in out Context;
      Val_Reg     : String;
      From_Ty     : String;
      To_Ty       : String;
      Is_Unsigned : Boolean := False) return String;

   -- Emit runtime checks (Constraint_Error by default, or explicit exception tag)
   procedure Emit_Check_Fail_If (Ctx : in out Context; Cond_Reg : String; Tag : String := "");
   procedure Emit_Null_Check (Ctx : in out Context; Ptr_Reg : String);
   procedure Emit_Discriminant_Check
     (Ctx        : in out Context;
      Rec_Ptr    : String;
      Target_Rec : TREE;
      Is_Access  : Boolean := False);

   -- Emit runtime subtype range check on a scalar register against Ada_Type's bounds
   procedure Emit_Range_Check
     (Ctx        : in out Context;
      Val_Reg    : String;
      Val_Pir_Ty : String;
      Sub_Type   : TREE);

   -- Emit initialization of a composite aggregate or expression into Dst_Ptr
   procedure Emit_Init_Composite
     (Ctx          : in out Context;
      Dst_Ptr      : String;
      Dst_Type     : TREE;
      Init_Node    : TREE;
      Dst_Firsts   : Str_Array_4 := (others => Null_Unbounded_String);
      Dst_Lasts    : Str_Array_4 := (others => Null_Unbounded_String);
      Exact_Bounds : Boolean := False);

   -- Elaborate subtype/type constraints & pre-evaluate record component non-discriminant constraints (LRM 3.2.1/3.7.2)
   procedure Emit_Type_Elab_Checks
     (Ctx                 : in out Context;
      T                   : TREE;
      Depth               : Natural := 0;
      Save_Allowed        : Boolean := True;
      Mark_Type           : TREE := CONST_VOID;
      Has_Real_Constraint : Boolean := False);

   -- Unified scalar conversion (float/fixed/discrete, signedness, narrowing & optional range check)
   function Emit_Typed_Scalar_Convert
     (Ctx         : in out Context;
      Val_Reg     : String;
      Src_Type    : TREE;
      Src_Pir     : String;
      Dst_Type    : TREE;
      Dst_Pir     : String;
      Check_Range : Boolean := False) return String;

   -- Compute runtime 'CONSTRAINED i8 value (0 or 1) for an actual record expression
   function Emit_Actual_Constrained (Ctx : in out Context; Act : TREE) return String;

   -- Save/load a scalar value into/from a stack/global slot for reuse without re-evaluation
   function Load_Saved_Slot
     (Ctx     : in out Context;
      Ptr_Str : String;
      Pir_Ty  : String;
      Align   : Natural;
      Is_Glob : Boolean) return String;

   procedure Save_Scalar_Slot
     (Ctx         : in out Context;
      Val_Reg     : String;
      Pir_Ty      : String;
      Align       : Natural;
      Ptr_Out     : out Unbounded_String;
      Is_Glob_Out : out Boolean);

   -- Emit default initialization (discriminants, default record components, null access, tasks) into Dst_Ptr
   procedure Emit_Default_Init
     (Ctx            : in out Context;
      Dst_Ptr        : String;
      Dst_Type       : TREE;
      Activate_Tasks : Boolean := False);

   -- Create an Ada 83 task handle (ptr) for Task_Spec in current master
   function Emit_Create_Task_Handle
     (Ctx          : in out Context;
      Task_Spec    : TREE;
      Activate_Now : Boolean := False) return String;

   -- Evaluate a Duration expression into signed 64-bit nanoseconds (i64)
   function Emit_Duration_Ns
     (Ctx     : in out Context;
      Dur_Exp : TREE) return String;

   -- True if Node denotes a pragma SHARED variable (walking SM_RENAMES_OBJ and Var_Info.Is_Shared)
   function Is_Shared_Object (Ctx : Context; Node : TREE) return Boolean;

   -- Compute i32 entry index for EI (and optional family index Fam_Exp), honoring saved dynamic bounds (P3-4.10)
   function Emit_Entry_Index_Reg
     (Ctx     : in out Context;
      EI      : Entry_Info;
      Fam_Exp : TREE) return String;

   -- Emit composite copy from Src to Dst
   procedure Emit_Copy_Composite
     (Ctx      : in out Context;
      Dst_Ptr  : String;
      Src      : Expr_Val;
      Dst_Type : TREE);

   -- Bitfield helpers for record representation clauses (LRM 13.4)
   function Is_Bitfield (FE : Field_Entry) return Boolean;

   procedure Emit_Variant_Check_For_Field
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_T     : TREE;
      Field_Def : TREE;
      Use_CK    : Unsigned_32);

   function Emit_Bitfield_Load
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      FE        : Field_Entry;
      Comp_Type : TREE) return String;

   procedure Emit_Bitfield_Store
     (Ctx          : in out Context;
      Rec_Ptr      : String;
      FE           : Field_Entry;
      Val_Reg      : String;
      Val_Pir_Type : String;
      Comp_Type    : TREE);

   -- Emit a subprogram call (shared by function calls and procedure calls)
   function Emit_Subp_Call
     (Ctx      : in out Context;
      Call_Node : TREE;
      Is_Func  : Boolean) return Expr_Val;

   -- Normalized view of any array value (EK_Slice or constrained EK_Addr), with 64-bit bounds
   type Array_View is record
      Data_Ptr   : Unbounded_String;
      Ada_Type   : TREE := (0, 0, 0);
      Elem_Type  : TREE := (0, 0, 0);
      Elem_Size  : Natural := 1;
      Elem_Align : Natural := 1;
      NDims      : Natural := 1;
      Firsts     : Str_Array_4; -- i64 bound registers
      Lasts      : Str_Array_4; -- i64 bound registers
   end record;

   function Resolve_Array_View (Ctx : in out Context; V : Expr_Val) return Array_View;

   -- Populate / unpack a @__ada_arr_desc (or @__ada_arr_desc_<ND>) struct with ptr + i64 bounds
   procedure Emit_Store_Arr_Desc
     (Ctx      : in out Context;
      Desc_Ptr : String;
      Data_Ptr : String;
      Firsts   : Str_Array_4;
      Lasts    : Str_Array_4;
      ND       : Natural;
      Desc_Ty  : String := "@__ada_arr_desc");

   procedure Emit_Unpack_Arr_Desc
     (Ctx      : in out Context;
      Desc_Ptr : String;
      Des_T    : TREE;
      Out_Val  : out Expr_Val;
      Desc_Ty  : String := "@__ada_arr_desc");

   -- Check exact i64 First/Last bounds equality across dims 1..NDims (raises Constraint_Error on mismatch)
   procedure Emit_Exact_Bounds_Check
     (Ctx    : in out Context;
      F1, L1 : Str_Array_4;
      F2, L2 : Str_Array_4;
      NDims  : Natural);

   -- Check that non-null i64 bounds [First_64 .. Last_64] belong to discrete subtype Idx_Type
   procedure Emit_Non_Null_Index_Check
     (Ctx      : in out Context;
      First_64 : String;
      Last_64  : String;
      Idx_Type : TREE);

   -- Compute array bounds (First_Reg, Last_Reg as i64) for a given array Expr_Val & dimension (1-based)
   procedure Get_Array_Dim_Bounds
     (Ctx       : in out Context;
      Arr       : Expr_Val;
      Dim       : Natural;
      First_Reg : out Unbounded_String;
      Last_Reg  : out Unbounded_String);

   -- Emit discrete index subtype bounds (First_R, Last_R as i64) for static or dynamic discrete type
   procedure Emit_Index_Bounds
     (Ctx      : in out Context;
      Idx_Type : TREE;
      First_R  : out Unbounded_String;
      Last_R   : out Unbounded_String);

   -- Compute 64-bit overflow-free dimension length: (Last >= First ? (Last - First + 1) : 0)
   function Emit_Dim_Len_I64
     (Ctx      : in out Context;
      First_64 : String;
      Last_64  : String) return String;

   -- Compute super-null-safe, overflow-checked total element count & byte size (raises STORAGE_ERROR on overflow)
   procedure Emit_Array_Byte_Size_I64
     (Ctx       : in out Context;
      Firsts    : Str_Array_4;
      Lasts     : Str_Array_4;
      NDims     : Natural;
      Elem_Sz   : Natural;
      Tot_Elems : out Unbounded_String;
      Tot_Bytes : out Unbounded_String);

   -- Emit Ada 83 LRM 5.2.1(2) array length check across dims 1..NDims (with null-to-null allowance)
   procedure Emit_Array_Length_Check
     (Ctx   : in out Context;
      LHS   : Expr_Val;
      RHS   : Expr_Val;
      NDims : Natural);

   -- Emit discrete range endpoints (Low, High) in Pir_Ty for a DN_RANGE / DN_DISCRETE_SUBTYPE / DN_RANGE_ATTRIBUTE
   procedure Emit_Discrete_Range
     (Ctx      : in out Context;
      Rng_Node : TREE;
      Pir_Ty   : out Unbounded_String;
      Low_Reg  : out Unbounded_String;
      High_Reg : out Unbounded_String);

private

   -- Internal helpers shared with Cg_Expr child packages
   function Format_Int_Imm_For_Pir (Val : Integer_64; Pir_Ty : String) return String;
   function Min_Signed_For_Pir (Pir_Ty : String) return Long_Long_Integer;

   function Emit_F64_To_Fixed_Ticks
     (Ctx     : in out Context;
      F_Reg   : String;
      F_Pir   : String;
      Fixed_T : TREE;
      Dst_Pir : String) return String;

   function Is_Access_To_Uncon_Array (T : TREE) return Boolean;
   function Expr_Has_Unbound_Discr (Ctx : Context; Node : TREE; Depth : Natural := 0) return Boolean;

   procedure Emit_Subtypes_Match_Check
     (Ctx   : in out Context;
      Src_T : TREE;
      Dst_T : TREE;
      Depth : Natural := 0);

   procedure Ensure_Enum_Tables_Pkg
     (Ctx       : in out Context;
      Base_Enum : TREE;
      Ptrs_Sym  : out Unbounded_String;
      Count     : out Natural);

   function Find_Enum_Rep_Type (Ctx : Context; T : TREE) return TREE;
   function Enum_Rep_Has_Negative (BT : TREE) return Boolean;
   function Emit_Enum_Rep_Table (Ctx : in out Context; BT : TREE; Count : out Natural) return String;
   function Emit_Enum_Pos_To_Rep_I64
     (Ctx    : in out Context;
      BT     : TREE;
      Val_R  : String;
      Pir_Ty : String) return String;
   function Emit_Enum_Rep_I64_To_Pos
     (Ctx     : in out Context;
      BT      : TREE;
      Rep_I64 : String) return String;

   function Emit_Field_Addr
     (Ctx     : in out Context;
      Rec_Ptr : String;
      Rec_Pir : String;
      FE      : Field_Entry;
      Prefix  : String := "fp") return String;

   function Emit_Load_Field_Scalar
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Comp_Type : TREE;
      Comp_TI   : Type_Info;
      Prefix    : String := "fv") return String;

   procedure Emit_Store_Field_Scalar
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Val_Reg   : String;
      Comp_Type : TREE;
      Comp_TI   : Type_Info);

   function Emit_Load_Field_I64
     (Ctx       : in out Context;
      Rec_Ptr   : String;
      Rec_Pir   : String;
      FE        : Field_Entry;
      Comp_Type : TREE;
      Comp_TI   : Type_Info) return String;

   function Emit_Variant_Choice_Match
     (Ctx       : in out Context;
      DV64      : String;
      Disc_Type : TREE;
      Choice_S  : TREE;
      Is_Others : out Boolean) return String;

   procedure Emit_Record_Rep_Copy_Into
     (Ctx        : in out Context;
      Dst_Ptr    : String;
      Dst_T      : TREE;
      Src_Ptr    : String;
      Src_T      : TREE;
      Discs_Only : Boolean := False);

end Cg_Expr;
