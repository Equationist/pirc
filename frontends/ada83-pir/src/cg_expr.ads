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

   -- Scalar conversion with fixed-point scaling & pre-fptosi overflow check (LRM 4.6, 9.5)
   function Emit_Typed_Scalar_Convert
     (Ctx      : in out Context;
      Val_Reg  : String;
      Src_Type : TREE;
      Src_Pir  : String;
      Dst_Type : TREE;
      Dst_Pir  : String) return String;

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

   -- Compute array bounds (First_Reg, Last_Reg as i32) for a given array Expr_Val & dimension (1-based)
   procedure Get_Array_Dim_Bounds
     (Ctx       : in out Context;
      Arr       : Expr_Val;
      Dim       : Natural;
      First_Reg : out Unbounded_String;
      Last_Reg  : out Unbounded_String);

   -- Emit discrete index subtype bounds (First_R, Last_R as i32) for static or dynamic discrete type
   procedure Emit_Index_Bounds
     (Ctx      : in out Context;
      Idx_Type : TREE;
      First_R  : out Unbounded_String;
      Last_R   : out Unbounded_String);

   -- Compute 64-bit overflow-free dimension length: (Last >= First ? (i64(Last) - i64(First) + 1) : 0)
   function Emit_Dim_Len_I64
     (Ctx      : in out Context;
      First_32 : String;
      Last_32  : String) return String;

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

end Cg_Expr;
