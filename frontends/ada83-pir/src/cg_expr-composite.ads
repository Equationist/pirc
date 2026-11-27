-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

private package Cg_Expr.Composite is

   procedure Emit_Copy_Composite
     (Ctx       : in out Context;
      Dst_Ptr   : String;
      Src       : Expr_Val;
      Dst_Type  : TREE;
      Max_Align : Natural := 8);

   procedure Emit_Init_Composite
     (Ctx          : in out Context;
      Dst_Ptr      : String;
      Dst_Type     : TREE;
      Init_Node    : TREE;
      Dst_Firsts   : Str_Array_4 := (others => Null_Unbounded_String);
      Dst_Lasts    : Str_Array_4 := (others => Null_Unbounded_String);
      Exact_Bounds : Boolean := False;
      Max_Align    : Natural := 8);

   procedure Emit_Default_Init
     (Ctx            : in out Context;
      Dst_Ptr        : String;
      Dst_Type       : TREE;
      Activate_Tasks : Boolean := False;
      Max_Align      : Natural := 8);

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
      Discs_Only : Boolean := False;
      Max_Align  : Natural := 8);

end Cg_Expr.Composite;
