-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Unbounded; use Ada.Strings.Unbounded;
with Interfaces; use Interfaces;
with USERPK; use USERPK;
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

   -- Emit scalar cast/widening/narrowing between PIR types (with optional range check)
   function Emit_Int_Convert
     (Ctx      : in out Context;
      Val_Reg  : String;
      From_Ty  : String;
      To_Ty    : String) return String;

   -- Emit runtime subtype range check on a scalar register against Ada_Type's bounds
   procedure Emit_Range_Check
     (Ctx        : in out Context;
      Val_Reg    : String;
      Val_Pir_Ty : String;
      Sub_Type   : TREE);

   -- Emit initialization of a composite aggregate or expression into Dst_Ptr
   procedure Emit_Init_Composite
     (Ctx        : in out Context;
      Dst_Ptr    : String;
      Dst_Type   : TREE;
      Init_Node  : TREE;
      Dst_Firsts : Str_Array_4 := (others => Null_Unbounded_String);
      Dst_Lasts  : Str_Array_4 := (others => Null_Unbounded_String));

   -- Emit composite copy from Src to Dst
   procedure Emit_Copy_Composite
     (Ctx      : in out Context;
      Dst_Ptr  : String;
      Src      : Expr_Val;
      Dst_Type : TREE);

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

   -- Emit discrete range endpoints (Low, High) in Pir_Ty for a DN_RANGE / DN_DISCRETE_SUBTYPE / DN_RANGE_ATTRIBUTE
   procedure Emit_Discrete_Range
     (Ctx      : in out Context;
      Rng_Node : TREE;
      Pir_Ty   : out Unbounded_String;
      Low_Reg  : out Unbounded_String;
      High_Reg : out Unbounded_String);

end Cg_Expr;
