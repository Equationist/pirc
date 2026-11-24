-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Strings.Fixed;
with DIANA; use DIANA;
with DIANAMAN; use DIANAMAN;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;

package body Cg_Expr is

   function Int_Bits_Of_Pir (Pir_Ty : String) return Natural is
   begin
      if Pir_Ty = "i8" then return 8;
      elsif Pir_Ty = "i16" then return 16;
      elsif Pir_Ty = "i32" then return 32;
      else return 64;
      end if;
   end Int_Bits_Of_Pir;

   function Min_Signed_For_Pir (Pir_Ty : String) return Long_Long_Integer is
   begin
      if Pir_Ty = "i8" then return -128;
      elsif Pir_Ty = "i16" then return -32768;
      elsif Pir_Ty = "i32" then return -2147483648;
      else return Long_Long_Integer'First;
      end if;
   end Min_Signed_For_Pir;

   function Emit_Int_Convert
     (Ctx      : in out Context;
      Val_Reg  : String;
      From_Ty  : String;
      To_Ty    : String) return String
   is
      FB : constant Natural := Int_Bits_Of_Pir (From_Ty);
      TB : constant Natural := Int_Bits_Of_Pir (To_Ty);
   begin
      if From_Ty = To_Ty then
         return Val_Reg;
      elsif From_Ty = "ptr" then
         declare
            R : constant String := Next_Reg (Ctx.E, "p2i");
         begin
            Emit_Inst (Ctx.E, R & " = ptrtoint." & To_Ty & " " & Val_Reg);
            return R;
         end;
      elsif To_Ty = "ptr" then
         declare
            R : constant String := Next_Reg (Ctx.E, "i2p");
         begin
            Emit_Inst (Ctx.E, R & " = inttoptr.ptr " & Val_Reg);
            return R;
         end;
      elsif TB > FB then
         declare
            R : constant String := Next_Reg (Ctx.E, "ext");
         begin
            Emit_Inst (Ctx.E, R & " = sext." & To_Ty & " " & Val_Reg);
            return R;
         end;
      else
         declare
            R : constant String := Next_Reg (Ctx.E, "tr");
         begin
            Emit_Inst (Ctx.E, R & " = trunc." & To_Ty & " " & Val_Reg);
            return R;
         end;
      end if;
   end Emit_Int_Convert;

   procedure Emit_Check_Fail_If (Ctx : in out Context; Cond_Reg : String) is
      Fail_L : constant String := Next_Label (Ctx.E, "chk_fail");
      Ok_L   : constant String := Next_Label (Ctx.E, "chk_ok");
   begin
      Emit_Term (Ctx.E, "br " & Cond_Reg & ", " & Fail_L & ", " & Ok_L);
      Emit_Label (Ctx.E, Fail_L);
      Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
      Emit_Label (Ctx.E, Ok_L);
   end Emit_Check_Fail_If;

   procedure Emit_Range_Check
     (Ctx        : in out Context;
      Val_Reg    : String;
      Val_Pir_Ty : String;
      Sub_Type   : TREE)
   is
      R  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Sub_Type));
      St : Boolean;
      Lo, Hi : Integer_64;
   begin
      if not Ctx.Enable_Checks or else Is_Void_Tree (R) then
         return;
      end if;
      if not Is_Discrete_Type (R) then
         return;
      end if;
      Get_Discrete_Bounds (R, St, Lo, Hi);
      if not St then
         -- Dynamic range check if DN_RANGE exists
         if KIND (R) = DN_INTEGER or else KIND (R) = DN_ENUMERATION then
            declare
               Rng : constant TREE := D (SM_RANGE, R);
            begin
               if not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                  declare
                     E1 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng));
                     E2 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng));
                     L_R : constant String := Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), Val_Pir_Ty);
                     H_R : constant String := Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), Val_Pir_Ty);
                     C1  : constant String := Next_Reg (Ctx.E, "rlo");
                     C2  : constant String := Next_Reg (Ctx.E, "rhi");
                     Bad : constant String := Next_Reg (Ctx.E, "rbad");
                  begin
                     Emit_Inst (Ctx.E, C1 & " = cmp_lt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & L_R);
                     Emit_Inst (Ctx.E, C2 & " = cmp_gt_s." & Val_Pir_Ty & " " & Val_Reg & ", " & H_R);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;
            end;
         end if;
         return;
      end if;

      -- Skip tautological full-width checks (e.g. full Integer range on i32)
      declare
         Bits : constant Natural := Int_Bits_Of_Pir (Val_Pir_Ty);
         Full_Lo : Integer_64;
         Full_Hi : Integer_64;
      begin
         case Bits is
            when 8  => Full_Lo := -128; Full_Hi := 127;
            when 16 => Full_Lo := -32768; Full_Hi := 32767;
            when 32 => Full_Lo := -2147483648; Full_Hi := 2147483647;
            when others => Full_Lo := Integer_64'First; Full_Hi := Integer_64'Last;
         end case;
         if Lo <= Full_Lo and then Hi >= Full_Hi then
            return;
         end if;
         -- For unsigned-like enums (e.g. Boolean 0..1, Character 0..127) on i8:
         declare
            Need_Lo : constant Boolean := Lo > Full_Lo;
            Need_Hi : constant Boolean := Hi < Full_Hi;
            C1, C2, Bad : Unbounded_String;
         begin
            if Need_Lo and then Need_Hi then
               C1  := To_Unbounded_String (Next_Reg (Ctx.E, "rlo"));
               C2  := To_Unbounded_String (Next_Reg (Ctx.E, "rhi"));
               Bad := To_Unbounded_String (Next_Reg (Ctx.E, "rbad"));
               Emit_Inst (Ctx.E, To_String (C1) & " = cmp_lt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Lo)));
               Emit_Inst (Ctx.E, To_String (C2) & " = cmp_gt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Hi)));
               Emit_Inst (Ctx.E, To_String (Bad) & " = or.i8 " & To_String (C1) & ", " & To_String (C2));
               Emit_Check_Fail_If (Ctx, To_String (Bad));
            elsif Need_Lo then
               C1 := To_Unbounded_String (Next_Reg (Ctx.E, "rlo"));
               Emit_Inst (Ctx.E, To_String (C1) & " = cmp_lt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Lo)));
               Emit_Check_Fail_If (Ctx, To_String (C1));
            elsif Need_Hi then
               C2 := To_Unbounded_String (Next_Reg (Ctx.E, "rhi"));
               Emit_Inst (Ctx.E, To_String (C2) & " = cmp_gt_s." & Val_Pir_Ty & " " &
                 Val_Reg & ", " & Img (Long_Long_Integer (Hi)));
               Emit_Check_Fail_If (Ctx, To_String (C2));
            end if;
         end;
      end;
   end Emit_Range_Check;

   procedure Get_Array_Dim_Bounds
     (Ctx       : in out Context;
      Arr       : Expr_Val;
      Dim       : Natural;
      First_Reg : out Unbounded_String;
      Last_Reg  : out Unbounded_String)
   is
   begin
      if Arr.Kind = EK_Slice and then Dim >= 1 and then Dim <= Arr.NDims then
         First_Reg := Arr.First_Regs (Dim);
         Last_Reg  := Arr.Last_Regs (Dim);
         return;
      end if;
      declare
         TI : constant Type_Info := Get_Type_Info (Ctx, Arr.Ada_Type);
      begin
         if Dim >= 1 and then Dim <= TI.NDims and then TI.Dims (Dim).Is_Static then
            declare
               RF : constant String := Next_Reg (Ctx.E, "af");
               RL : constant String := Next_Reg (Ctx.E, "al");
            begin
               Emit_Inst (Ctx.E, RF & " = const.i32 " & Img (Long_Long_Integer (TI.Dims (Dim).First)));
               Emit_Inst (Ctx.E, RL & " = const.i32 " & Img (Long_Long_Integer (TI.Dims (Dim).Last)));
               First_Reg := To_Unbounded_String (RF);
               Last_Reg  := To_Unbounded_String (RL);
            end;
            return;
         elsif Dim >= 1 and then Dim <= Arr.NDims and then Length (Arr.First_Regs (Dim)) > 0 then
            First_Reg := Arr.First_Regs (Dim);
            Last_Reg  := Arr.Last_Regs (Dim);
            return;
         end if;
      end;
      declare
         RF : constant String := Next_Reg (Ctx.E, "af");
         RL : constant String := Next_Reg (Ctx.E, "al");
      begin
         Emit_Inst (Ctx.E, RF & " = const.i32 1");
         Emit_Inst (Ctx.E, RL & " = const.i32 0");
         First_Reg := To_Unbounded_String (RF);
         Last_Reg  := To_Unbounded_String (RL);
      end;
   end Get_Array_Dim_Bounds;

   procedure Emit_Discrete_Range
     (Ctx      : in out Context;
      Rng_Node : TREE;
      Pir_Ty   : out Unbounded_String;
      Low_Reg  : out Unbounded_String;
      High_Reg : out Unbounded_String)
   is
      K : constant NODE_NAME := KIND (Rng_Node);
   begin
      if K = DN_RANGE then
         declare
            T  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Rng_Node));
            TI : constant Type_Info := Get_Type_Info (Ctx, T);
            E1 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng_Node), T);
            E2 : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng_Node), T);
         begin
            Pir_Ty := TI.Pir_Type;
            Low_Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), To_String (Pir_Ty)));
            High_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), To_String (Pir_Ty)));
         end;
      elsif K = DN_DISCRETE_SUBTYPE then
         declare
            SI : constant TREE := D (AS_SUBTYPE_INDICATION, Rng_Node);
         begin
            if KIND (SI) = DN_SUBTYPE_INDICATION then
               declare
                  C : constant TREE := D (AS_CONSTRAINT, SI);
               begin
                  if not Is_Void_Tree (C) and then KIND (C) = DN_RANGE then
                     Emit_Discrete_Range (Ctx, C, Pir_Ty, Low_Reg, High_Reg);
                     return;
                  end if;
               end;
               declare
                  Def : constant TREE := Get_Name_Defn (D (AS_NAME, SI));
                  TS  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def));
                  TI  : constant Type_Info := Get_Type_Info (Ctx, TS);
                  St  : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Pir_Ty := TI.Pir_Type;
                  Get_Discrete_Bounds (TS, St, Lo, Hi);
                  if St then
                     declare
                        RL : constant String := Next_Reg (Ctx.E, "lo");
                        RH : constant String := Next_Reg (Ctx.E, "hi");
                     begin
                        Emit_Inst (Ctx.E, RL & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Lo)));
                        Emit_Inst (Ctx.E, RH & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Hi)));
                        Low_Reg  := To_Unbounded_String (RL);
                        High_Reg := To_Unbounded_String (RH);
                     end;
                  else
                     Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (TS)), Pir_Ty, Low_Reg, High_Reg);
                  end if;
               end;
            else
               declare
                  Def : constant TREE := Get_Name_Defn (SI);
                  TS  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Def));
                  TI  : constant Type_Info := Get_Type_Info (Ctx, TS);
                  St  : Boolean;
                  Lo, Hi : Integer_64;
               begin
                  Pir_Ty := TI.Pir_Type;
                  Get_Discrete_Bounds (TS, St, Lo, Hi);
                  if St then
                     declare
                        RL : constant String := Next_Reg (Ctx.E, "lo");
                        RH : constant String := Next_Reg (Ctx.E, "hi");
                     begin
                        Emit_Inst (Ctx.E, RL & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Lo)));
                        Emit_Inst (Ctx.E, RH & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (Hi)));
                        Low_Reg  := To_Unbounded_String (RL);
                        High_Reg := To_Unbounded_String (RH);
                     end;
                  else
                     Emit_Discrete_Range (Ctx, D (SM_RANGE, Resolve_Type_Spec (TS)), Pir_Ty, Low_Reg, High_Reg);
                  end if;
               end;
            end if;
         end;
      elsif K = DN_RANGE_ATTRIBUTE then
         declare
            Pfx : constant TREE := D (AS_NAME, Rng_Node);
            Dim : Natural := 1;
            Exp : constant TREE := D (AS_EXP, Rng_Node);
         begin
            if not Is_Void_Tree (Exp) and then Has_Static_Val (D (SM_VALUE, Exp)) then
               Dim := Natural (Eval_Int64 (D (SM_VALUE, Exp)));
            end if;
            -- Prefix can be an array object or an array type mark
            if KIND (Pfx) = DN_USED_NAME_ID and then
              KIND (D (SM_DEFN, Pfx)) in CLASS_TYPE_NAME
            then
               declare
                  TS : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (SM_DEFN, Pfx)));
                  TI : constant Type_Info := Get_Type_Info (Ctx, TS);
                  IT : constant TREE := TI.Dims (Dim).Idx_Type;
                  ITI : constant Type_Info := Get_Type_Info (Ctx, IT);
                  RL : constant String := Next_Reg (Ctx.E, "lo");
                  RH : constant String := Next_Reg (Ctx.E, "hi");
               begin
                  Pir_Ty := ITI.Pir_Type;
                  Emit_Inst (Ctx.E, RL & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (TI.Dims (Dim).First)));
                  Emit_Inst (Ctx.E, RH & " = const." & To_String (Pir_Ty) & " " & Img (Long_Long_Integer (TI.Dims (Dim).Last)));
                  Low_Reg  := To_Unbounded_String (RL);
                  High_Reg := To_Unbounded_String (RH);
               end;
            else
               declare
                  Arr : constant Expr_Val := Emit_LValue (Ctx, Pfx);
                  F32, L32 : Unbounded_String;
                  TI  : constant Type_Info := Get_Type_Info (Ctx, Arr.Ada_Type);
                  IT  : constant TREE := TI.Dims (Dim).Idx_Type;
                  ITI : constant Type_Info := Get_Type_Info (Ctx, IT);
               begin
                  Get_Array_Dim_Bounds (Ctx, Arr, Dim, F32, L32);
                  Pir_Ty := ITI.Pir_Type;
                  Low_Reg  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F32), "i32", To_String (Pir_Ty)));
                  High_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L32), "i32", To_String (Pir_Ty)));
               end;
            end if;
         end;
      else
         Pir_Ty   := To_Unbounded_String ("i32");
         Low_Reg  := To_Unbounded_String ("0");
         High_Reg := To_Unbounded_String ("0");
      end if;
   end Emit_Discrete_Range;

   -- Emit a null pointer check on an access value
   procedure Emit_Null_Check (Ctx : in out Context; Ptr_Reg : String) is
   begin
      if not Ctx.Enable_Checks then
         return;
      end if;
      declare
         Is_Null : constant String := Next_Reg (Ctx.E, "nchk");
      begin
         Emit_Inst (Ctx.E, Is_Null & " = cmp_eq.ptr " & Ptr_Reg & ", 0");
         Emit_Check_Fail_If (Ctx, Is_Null);
      end;
   end Emit_Null_Check;

   function Emit_LValue (Ctx : in out Context; Node : TREE) return Expr_Val is
      Res : Expr_Val;
      K   : constant NODE_NAME := KIND (Node);
   begin
      case K is
         when DN_USED_OBJECT_ID | DN_USED_NAME_ID =>
            declare
               Raw_Def : constant TREE := D (SM_DEFN, Node);
               Def     : constant TREE := Canonical_Def (Subst_Tree (Ctx, Raw_Def));
               Key     : constant Unsigned_32 := Tree_Key (Def);
            begin
               if Ctx.Vars.Contains (Key) then
                  declare
                     V  : constant Var_Info := Ctx.Vars.Element (Key);
                     ATy : constant TREE := Subst_Tree (Ctx, V.Ada_Type);
                     TI : constant Type_Info := Get_Type_Info (Ctx, ATy);
                  begin
                     Res.Ada_Type := ATy;
                     Res.Align    := V.Align;
                     Res.Pir_Type := TI.Pir_Type;
                     if V.Kind = VK_Uncon_Array_Param or else V.Kind = VK_Dyn_Array then
                        Res.Kind       := EK_Slice;
                        Res.Reg        := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                        Res.NDims      := V.NDims;
                        Res.First_Regs := V.First_Regs;
                        Res.Last_Regs  := V.Last_Regs;
                     elsif V.Kind = VK_Param_Val then
                        -- Shouldn't be modified, but return value
                        Res.Kind := EK_Scalar;
                        Res.Reg  := V.Pir_Name;
                     else
                        Res.Kind := EK_Addr;
                        Res.Reg  := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                     end if;
                     return Res;
                  end;
               else
                  -- External package variable from a with'd unit
                  declare
                     ATy  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def));
                     TI   : constant Type_Info := Get_Type_Info (Ctx, ATy);
                     GSym : constant String := "@ada_g__" & Qualified_Ada_Name (Def);
                     AR   : constant String := Next_Reg (Ctx.E, "ega");
                  begin
                     Add_Global_Decl (Ctx.E, GSym,
                       "extern " & GSym & " = global " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                     Emit_Inst (Ctx.E, AR & " = addr " & GSym);
                     Res.Kind     := EK_Addr;
                     Res.Reg      := To_Unbounded_String (AR);
                     Res.Pir_Type := TI.Pir_Type;
                     Res.Ada_Type := ATy;
                     Res.Align    := TI.Align;
                     return Res;
                  end;
               end if;
            end;

         when DN_SELECTED =>
            declare
               Pfx   : constant TREE := D (AS_NAME, Node);
               Des   : constant TREE := D (AS_DESIGNATOR, Node);
               Def   : constant TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Des)));
            begin
               -- Package.Object?
               if KIND (Def) /= DN_COMPONENT_ID and then KIND (Def) /= DN_DISCRIMINANT_ID then
                  return Emit_LValue (Ctx, Des);
               end if;
               -- Record component access (with implicit access dereference if Pfx is access type!)
               declare
                  Base_LV : Expr_Val := Emit_LValue (Ctx, Pfx);
                  Pfx_T   : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Base_LV.Ada_Type));
                  Rec_Ptr : Unbounded_String := Base_LV.Reg;
                  Rec_T   : TREE := Pfx_T;
               begin
                  if Is_Access_Type (Pfx_T) then
                     if Base_LV.Kind = EK_Addr then
                        declare
                           LP : constant String := Next_Reg (Ctx.E, "ap");
                        begin
                           Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8");
                           Rec_Ptr := To_Unbounded_String (LP);
                        end;
                     end if;
                     Emit_Null_Check (Ctx, To_String (Rec_Ptr));
                     Rec_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T)));
                  end if;
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, Rec_T);
                     CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def));
                     CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                     CK  : constant Unsigned_32 := Tree_Key (Def);
                     FP  : constant String := Next_Reg (Ctx.E, "fp");
                  begin
                     if Ctx.Fields.Contains (CK) then
                        declare
                           FE : constant Field_Entry := Ctx.Fields.Element (CK);
                        begin
                           if not FE.In_Union then
                              Emit_Inst (Ctx.E,
                                FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " &
                                To_String (Rec_Ptr) & ", " & Img (FE.Field_Idx));
                           else
                              declare
                                 UP : constant String := Next_Reg (Ctx.E, "up");
                                 VP : constant String := Next_Reg (Ctx.E, "vp");
                              begin
                                 Emit_Inst (Ctx.E,
                                   UP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " &
                                   To_String (Rec_Ptr) & ", " & Img (FE.Union_Idx));
                                 Emit_Inst (Ctx.E,
                                   VP & " = field_ptr " & To_String (RTI.Pir_Type) & "_u" &
                                   Img (FE.Union_Idx) & ", " & UP & ", " & Img (FE.Var_Idx));
                                 Emit_Inst (Ctx.E,
                                   FP & " = field_ptr " & To_String (FE.Var_Type) & ", " &
                                   VP & ", " & Img (FE.Var_Field));
                              end;
                           end if;
                        end;
                     else
                        Emit_Inst (Ctx.E,
                          FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " &
                          To_String (Rec_Ptr) & ", 0");
                     end if;
                     Res.Kind     := EK_Addr;
                     Res.Reg      := To_Unbounded_String (FP);
                     Res.Pir_Type := CTI.Pir_Type;
                     Res.Ada_Type := CT;
                     Res.Align    := CTI.Align;
                     return Res;
                  end;
               end;
            end;

         when DN_ALL =>
            declare
               Pfx   : constant Expr_Val := Emit_Expr (Ctx, D (AS_NAME, Node));
               Pfx_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Pfx.Ada_Type));
               Des_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_T)));
               DTI   : constant Type_Info := Get_Type_Info (Ctx, Des_T);
            begin
               Emit_Null_Check (Ctx, To_String (Pfx.Reg));
               Res.Kind     := EK_Addr;
               Res.Reg      := Pfx.Reg;
               Res.Pir_Type := DTI.Pir_Type;
               Res.Ada_Type := Des_T;
               Res.Align    := DTI.Align;
               return Res;
            end;

         when DN_INDEXED =>
            declare
               Pfx_Node : constant TREE := D (AS_NAME, Node);
               Arr_Val  : Expr_Val;
               Arr_Ptr  : Unbounded_String;
               Arr_T    : TREE;
            begin
               if KIND (Pfx_Node) = DN_FUNCTION_CALL
                 or else KIND (Pfx_Node) = DN_NULLARY_CALL
                 or else KIND (Pfx_Node) = DN_SLICE
               then
                  Arr_Val := Emit_Expr (Ctx, Pfx_Node);
               else
                  Arr_Val := Emit_LValue (Ctx, Pfx_Node);
               end if;
               Arr_Ptr := Arr_Val.Reg;
               Arr_T   := Resolve_Type_Spec (Subst_Tree (Ctx, Arr_Val.Ada_Type));
               if Is_Access_Type (Arr_T) then
                  if Arr_Val.Kind = EK_Addr then
                     declare
                        LP : constant String := Next_Reg (Ctx.E, "ap");
                     begin
                        Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Arr_Ptr) & ", align 8");
                        Arr_Ptr := To_Unbounded_String (LP);
                     end;
                  end if;
                  Emit_Null_Check (Ctx, To_String (Arr_Ptr));
                  Arr_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Arr_T)));
                  Arr_Val.Ada_Type := Arr_T;
               end if;

               declare
                  ATI     : constant Type_Info := Get_Type_Info (Ctx, Arr_T);
                  ETI     : constant Type_Info := Get_Type_Info (Ctx, ATI.Elem_Type);
                  Exps    : SEQ_TYPE := LIST (D (AS_EXP_S, Node));
                  Exp_N   : TREE;
                  Dim     : Natural := 0;
                  Cur_Ptr : Unbounded_String := Arr_Ptr;
                  Flat    : Unbounded_String;
               begin
                  while not IS_EMPTY (Exps) loop
                     POP (Exps, Exp_N);
                     Dim := Dim + 1;
                     declare
                        IV    : constant Expr_Val := Emit_Expr (Ctx, Exp_N);
                        I32   : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i32");
                        F_Reg : Unbounded_String;
                        L_Reg : Unbounded_String;
                        Off32 : constant String := Next_Reg (Ctx.E, "ioff");
                        Off64 : constant String := Next_Reg (Ctx.E, "ioff64");
                     begin
                        Get_Array_Dim_Bounds (Ctx, Arr_Val, Dim, F_Reg, L_Reg);
                        declare
                           Skip_Chk : Boolean := not Ctx.Enable_Checks;
                        begin
                           if not Skip_Chk and then Dim <= ATI.NDims and then ATI.Dims (Dim).Is_Static then
                              if Has_Static_Val (D (SM_VALUE, Exp_N)) then
                                 declare
                                    SV : constant Integer_64 := Eval_Int64 (D (SM_VALUE, Exp_N));
                                 begin
                                    if SV >= ATI.Dims (Dim).First and then SV <= ATI.Dims (Dim).Last then
                                       Skip_Chk := True;
                                    end if;
                                 end;
                              elsif KIND (Exp_N) = DN_USED_OBJECT_ID then
                                 declare
                                    DK : constant Unsigned_32 := Tree_Key (Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Exp_N))));
                                 begin
                                    if Ctx.Vars.Contains (DK) then
                                       declare
                                          IVar : constant Var_Info := Ctx.Vars.Element (DK);
                                       begin
                                          if IVar.Has_Static_Rng
                                            and then IVar.Rng_Lo >= ATI.Dims (Dim).First
                                            and then IVar.Rng_Hi <= ATI.Dims (Dim).Last
                                          then
                                             Skip_Chk := True;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                           end if;
                           if not Skip_Chk then
                              declare
                                 C1  : constant String := Next_Reg (Ctx.E, "ilo");
                                 C2  : constant String := Next_Reg (Ctx.E, "ihi");
                                 Bad : constant String := Next_Reg (Ctx.E, "ibad");
                              begin
                                 Emit_Inst (Ctx.E, C1 & " = cmp_lt_s.i32 " & I32 & ", " & To_String (F_Reg));
                                 Emit_Inst (Ctx.E, C2 & " = cmp_gt_s.i32 " & I32 & ", " & To_String (L_Reg));
                                 Emit_Inst (Ctx.E, Bad & " = or.i8 " & C1 & ", " & C2);
                                 Emit_Check_Fail_If (Ctx, Bad);
                              end;
                           end if;
                        end;
                        Emit_Inst (Ctx.E, Off32 & " = sub.i32 " & I32 & ", " & To_String (F_Reg));
                        Emit_Inst (Ctx.E, Off64 & " = sext.i64 " & Off32);
                        if Dim < ATI.NDims then
                           -- Advance base pointer by row stride (enables LICM hoisting out of inner dim loops!)
                           declare
                              Stride_Elems : Unbounded_String;
                              All_St : Boolean := True;
                              Prod   : Integer_64 := 1;
                           begin
                              for Sub_D in Dim + 1 .. ATI.NDims loop
                                 if ATI.Dims (Sub_D).Is_Static then
                                    if ATI.Dims (Sub_D).Last >= ATI.Dims (Sub_D).First then
                                       Prod := Prod * (ATI.Dims (Sub_D).Last - ATI.Dims (Sub_D).First + 1);
                                    else
                                       Prod := 0;
                                    end if;
                                 else
                                    All_St := False;
                                 end if;
                              end loop;
                              if All_St then
                                 declare
                                    R_Off : constant String := Next_Reg (Ctx.E, "roff");
                                    NP    : constant String := Next_Reg (Ctx.E, "rptr");
                                 begin
                                    Emit_Inst (Ctx.E, R_Off & " = mul.i64 " & Off64 & ", " & Img (Long_Long_Integer (Prod)));
                                    Emit_Inst (Ctx.E, NP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & To_String (Cur_Ptr) & ", " & R_Off);
                                    Cur_Ptr := To_Unbounded_String (NP);
                                 end;
                              else
                                 declare
                                    S_Acc : Unbounded_String := To_Unbounded_String (Off64);
                                 begin
                                    for Sub_D in Dim + 1 .. ATI.NDims loop
                                       declare
                                          SF, SL : Unbounded_String;
                                          Sp : constant String := Next_Reg (Ctx.E, "spn");
                                          Ln : constant String := Next_Reg (Ctx.E, "dlen");
                                          L6 : constant String := Next_Reg (Ctx.E, "dlen64");
                                          Ml : constant String := Next_Reg (Ctx.E, "fmul");
                                       begin
                                          Get_Array_Dim_Bounds (Ctx, Arr_Val, Sub_D, SF, SL);
                                          Emit_Inst (Ctx.E, Sp & " = sub.i32 " & To_String (SL) & ", " & To_String (SF));
                                          Emit_Inst (Ctx.E, Ln & " = add.i32 " & Sp & ", 1");
                                          Emit_Inst (Ctx.E, L6 & " = sext.i64 " & Ln);
                                          Emit_Inst (Ctx.E, Ml & " = mul.i64 " & To_String (S_Acc) & ", " & L6);
                                          S_Acc := To_Unbounded_String (Ml);
                                       end;
                                    end loop;
                                    declare
                                       NP : constant String := Next_Reg (Ctx.E, "rptr");
                                    begin
                                       Emit_Inst (Ctx.E, NP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & To_String (Cur_Ptr) & ", " & To_String (S_Acc));
                                       Cur_Ptr := To_Unbounded_String (NP);
                                    end;
                                 end;
                              end if;
                              pragma Unreferenced (Stride_Elems);
                           end;
                        else
                           Flat := To_Unbounded_String (Off64);
                        end if;
                     end;
                  end loop;

                  declare
                     EP : constant String := Next_Reg (Ctx.E, "ep");
                  begin
                     Emit_Inst (Ctx.E,
                       EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " &
                       To_String (Cur_Ptr) & ", " & To_String (Flat));
                     Res.Kind     := EK_Addr;
                     Res.Reg      := To_Unbounded_String (EP);
                     Res.Pir_Type := ETI.Pir_Type;
                     Res.Ada_Type := ATI.Elem_Type;
                     Res.Align    := ETI.Align;
                     return Res;
                  end;
               end;
            end;

         when DN_SLICE =>
            declare
               Pfx_Val : constant Expr_Val := Emit_LValue (Ctx, D (AS_NAME, Node));
               ATI     : constant Type_Info := Get_Type_Info (Ctx, Pfx_Val.Ada_Type);
               ETI     : constant Type_Info := Get_Type_Info (Ctx, ATI.Elem_Type);
               Rng_T, Lo_R, Hi_R : Unbounded_String;
               AF_R, AL_R : Unbounded_String;
               Lo32, Hi32 : Unbounded_String;
            begin
               Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Node), Rng_T, Lo_R, Hi_R);
               Lo32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Lo_R), To_String (Rng_T), "i32"));
               Hi32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Hi_R), To_String (Rng_T), "i32"));
               Get_Array_Dim_Bounds (Ctx, Pfx_Val, 1, AF_R, AL_R);

               if Ctx.Enable_Checks then
                  -- Non-empty slice check: if Lo <= Hi then Lo >= AF and Hi <= AL
                  declare
                     Non_Empty : constant String := Next_Reg (Ctx.E, "sne");
                     C1        : constant String := Next_Reg (Ctx.E, "slo");
                     C2        : constant String := Next_Reg (Ctx.E, "shi");
                     Oob       : constant String := Next_Reg (Ctx.E, "soob");
                     Bad       : constant String := Next_Reg (Ctx.E, "sbad");
                  begin
                     Emit_Inst (Ctx.E, Non_Empty & " = cmp_le_s.i32 " & To_String (Lo32) & ", " & To_String (Hi32));
                     Emit_Inst (Ctx.E, C1 & " = cmp_lt_s.i32 " & To_String (Lo32) & ", " & To_String (AF_R));
                     Emit_Inst (Ctx.E, C2 & " = cmp_gt_s.i32 " & To_String (Hi32) & ", " & To_String (AL_R));
                     Emit_Inst (Ctx.E, Oob & " = or.i8 " & C1 & ", " & C2);
                     Emit_Inst (Ctx.E, Bad & " = and.i8 " & Non_Empty & ", " & Oob);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;

               declare
                  Off32 : constant String := Next_Reg (Ctx.E, "soff");
                  Off64 : constant String := Next_Reg (Ctx.E, "soff64");
                  SP    : constant String := Next_Reg (Ctx.E, "sptr");
               begin
                  Emit_Inst (Ctx.E, Off32 & " = sub.i32 " & To_String (Lo32) & ", " & To_String (AF_R));
                  Emit_Inst (Ctx.E, Off64 & " = sext.i64 " & Off32);
                  Emit_Inst (Ctx.E, SP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & To_String (Pfx_Val.Reg) & ", " & Off64);
                  Res.Kind           := EK_Slice;
                  Res.Reg            := To_Unbounded_String (SP);
                  Res.Pir_Type       := To_Unbounded_String ("ptr");
                  Res.Ada_Type       := Base_Type_Spec (Pfx_Val.Ada_Type);
                  Res.Align          := ETI.Align;
                  Res.NDims          := 1;
                  Res.First_Regs (1) := Lo32;
                  Res.Last_Regs (1)  := Hi32;
                  return Res;
               end;
            end;

         when DN_PARENTHESIZED =>
            return Emit_LValue (Ctx, D (AS_EXP, Node));

         when others =>
            return Emit_Expr (Ctx, Node);
      end case;
   end Emit_LValue;

   procedure Emit_Copy_Composite
     (Ctx      : in out Context;
      Dst_Ptr  : String;
      Src      : Expr_Val;
      Dst_Type : TREE)
   is
      DTI : constant Type_Info := Get_Type_Info (Ctx, Dst_Type);
   begin
      if DTI.Kind = TK_Record or else (DTI.Kind = TK_Constrained_Array and then not DTI.Is_Dynamic) then
         if DTI.Byte_Size > 0 then
            Emit_Inst (Ctx.E,
              "memmove " & Dst_Ptr & ", " & To_String (Src.Reg) & ", " &
              Img (DTI.Byte_Size) & ", align " & Img (DTI.Align));
         end if;
      elsif DTI.Kind = TK_Unconstrained_Array or else DTI.Is_Dynamic then
         -- Copy based on Src's dynamic length across all dimensions
         declare
            ETI   : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
            ND    : constant Natural := (if DTI.NDims > 0 then DTI.NDims else 1);
            Tot64 : Unbounded_String;
            Byt   : constant String := Next_Reg (Ctx.E, "cby");
            ESz   : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
         begin
            for D_Idx in 1 .. ND loop
               declare
                  F, L : Unbounded_String;
                  Diff : constant String := Next_Reg (Ctx.E, "cd");
                  Len  : constant String := Next_Reg (Ctx.E, "cl");
                  Pos  : constant String := Next_Reg (Ctx.E, "cp");
                  CLen : constant String := Next_Reg (Ctx.E, "ccl");
                  L64  : constant String := Next_Reg (Ctx.E, "cl64");
               begin
                  Get_Array_Dim_Bounds (Ctx, Src, D_Idx, F, L);
                  Emit_Inst (Ctx.E, Diff & " = sub.i32 " & To_String (L) & ", " & To_String (F));
                  Emit_Inst (Ctx.E, Len & " = add.i32 " & Diff & ", 1");
                  Emit_Inst (Ctx.E, Pos & " = cmp_gt_s.i32 " & Len & ", 0");
                  Emit_Inst (Ctx.E, CLen & " = select.i32 " & Pos & ", " & Len & ", 0");
                  Emit_Inst (Ctx.E, L64 & " = sext.i64 " & CLen);
                  if D_Idx = 1 then
                     Tot64 := To_Unbounded_String (L64);
                  else
                     declare
                        M : constant String := Next_Reg (Ctx.E, "cm");
                     begin
                        Emit_Inst (Ctx.E, M & " = mul.i64 " & To_String (Tot64) & ", " & L64);
                        Tot64 := To_Unbounded_String (M);
                     end;
                  end if;
               end;
            end loop;
            Emit_Inst (Ctx.E, Byt & " = mul.i64 " & To_String (Tot64) & ", " & Img (ESz));
            Emit_Inst (Ctx.E, "memmove " & Dst_Ptr & ", " & To_String (Src.Reg) & ", " & Byt & ", align " & Img (ETI.Align));
         end;
      end if;
   end Emit_Copy_Composite;

   procedure Emit_Init_Composite
     (Ctx        : in out Context;
      Dst_Ptr    : String;
      Dst_Type   : TREE;
      Init_Node  : TREE;
      Dst_Firsts : Str_Array_4 := (others => Null_Unbounded_String);
      Dst_Lasts  : Str_Array_4 := (others => Null_Unbounded_String))
   is
      DTI : constant Type_Info := Get_Type_Info (Ctx, Dst_Type);
   begin
      if Is_Void_Tree (Init_Node) then
         return;
      end if;
      if KIND (Init_Node) = DN_QUALIFIED then
         Emit_Init_Composite (Ctx, Dst_Ptr, Dst_Type, D (AS_EXP, Init_Node), Dst_Firsts, Dst_Lasts);
         return;
      end if;
      if KIND (Init_Node) = DN_PARENTHESIZED then
         Emit_Init_Composite (Ctx, Dst_Ptr, Dst_Type, D (AS_EXP, Init_Node), Dst_Firsts, Dst_Lasts);
         return;
      end if;

      if KIND (Init_Node) = DN_AGGREGATE then
         if DTI.Kind = TK_Record then
            declare
               Norm : TREE := D (SM_NORMALIZED_COMP_S, Init_Node);
               Comps : SEQ_TYPE;
               C_Exp : TREE;
               Base_Rec : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, Dst_Type));
               -- Collect component IDs in field order (discriminants + comp_list)
               Comp_Ids : array (0 .. 127) of TREE;
               N_Comps  : Natural := 0;

               procedure Gather_Ids (DS : TREE) is
                  DL : SEQ_TYPE;
                  D_N : TREE;
               begin
                  if Is_Void_Tree (DS) then return; end if;
                  DL := LIST (DS);
                  while not IS_EMPTY (DL) loop
                     POP (DL, D_N);
                     if KIND (D_N) = DN_VARIABLE_DECL or else KIND (D_N) = DN_DSCRMT_DECL then
                        declare
                           NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, D_N));
                           ID : TREE;
                        begin
                           while not IS_EMPTY (NS) loop
                              POP (NS, ID);
                              if N_Comps < 128 then
                                 Comp_Ids (N_Comps) := ID;
                                 N_Comps := N_Comps + 1;
                              end if;
                           end loop;
                        end;
                     end if;
                  end loop;
               end Gather_Ids;
            begin
               Gather_Ids (D (SM_DISCRIMINANT_S, Base_Rec));
               if not Is_Void_Tree (D (SM_COMP_LIST, Base_Rec)) then
                  Gather_Ids (D (AS_DECL_S, D (SM_COMP_LIST, Base_Rec)));
               end if;
               if Is_Void_Tree (Norm) then
                  Norm := D (AS_GENERAL_ASSOC_S, Init_Node);
               end if;
               Comps := LIST (Norm);
               declare
                  Idx : Natural := 0;
               begin
                  while not IS_EMPTY (Comps) and then Idx < N_Comps loop
                     POP (Comps, C_Exp);
                     if KIND (C_Exp) = DN_ASSOC or else KIND (C_Exp) = DN_NAMED then
                        C_Exp := D (AS_EXP, C_Exp);
                     end if;
                     declare
                        CID : constant TREE := Comp_Ids (Idx);
                        CT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, CID));
                        CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                        FP  : constant String := Next_Reg (Ctx.E, "afp");
                     begin
                        Emit_Inst (Ctx.E,
                          FP & " = field_ptr " & To_String (DTI.Pir_Type) & ", " & Dst_Ptr & ", " & Img (Idx));
                        if CTI.Kind = TK_Scalar then
                           declare
                              V : constant Expr_Val := Emit_Expr (Ctx, C_Exp, CT);
                           begin
                              Emit_Range_Check (Ctx, To_String (V.Reg), To_String (V.Pir_Type), CT);
                              Emit_Inst (Ctx.E,
                                "store." & To_String (CTI.Pir_Type) & " " & To_String (V.Reg) &
                                ", " & FP & ", align " & Img (CTI.Align));
                           end;
                        else
                           Emit_Init_Composite (Ctx, FP, CT, C_Exp);
                        end if;
                     end;
                     Idx := Idx + 1;
                  end loop;
               end;
            end;
            return;

         elsif DTI.Kind = TK_Constrained_Array or else DTI.Kind = TK_Unconstrained_Array then
            declare
               ETI    : constant Type_Info := Get_Type_Info (Ctx, DTI.Elem_Type);
               Assocs : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, Init_Node));
               Item   : TREE;
               Pos    : Integer_64 := 0;
               Lo_Reg : Unbounded_String;
               Hi_Reg : Unbounded_String;
            begin
               if DTI.Kind = TK_Constrained_Array and then DTI.Dims (1).Is_Static then
                  declare
                     RF : constant String := Next_Reg (Ctx.E, "af");
                     RL : constant String := Next_Reg (Ctx.E, "al");
                  begin
                     Emit_Inst (Ctx.E, RF & " = const.i32 " & Img (Long_Long_Integer (DTI.Dims (1).First)));
                     Emit_Inst (Ctx.E, RL & " = const.i32 " & Img (Long_Long_Integer (DTI.Dims (1).Last)));
                     Lo_Reg := To_Unbounded_String (RF);
                     Hi_Reg := To_Unbounded_String (RL);
                  end;
               else
                  Lo_Reg := Dst_Firsts (1);
                  Hi_Reg := Dst_Lasts (1);
               end if;

               -- First pass: handle any "others => val" by filling entire array in a loop
               declare
                  Scan : SEQ_TYPE := Assocs;
                  SIt  : TREE;
               begin
                  while not IS_EMPTY (Scan) loop
                     POP (Scan, SIt);
                     if KIND (SIt) = DN_ASSOC or else KIND (SIt) = DN_NAMED then
                        declare
                           CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, SIt));
                           Ch : TREE;
                        begin
                           while not IS_EMPTY (CS) loop
                              POP (CS, Ch);
                              if KIND (Ch) = DN_CHOICE_OTHERS then
                                 -- Loop i from 0 to Total - 1
                                 declare
                                    Tot64 : Unbounded_String;
                                 begin
                                    if DTI.Kind = TK_Constrained_Array and then not DTI.Is_Dynamic then
                                       declare
                                          N_Tot : Integer_64 := 1;
                                          R_T   : constant String := Next_Reg (Ctx.E, "ntot");
                                       begin
                                          for D_Idx in 1 .. DTI.NDims loop
                                             if DTI.Dims (D_Idx).Last >= DTI.Dims (D_Idx).First then
                                                N_Tot := N_Tot * (DTI.Dims (D_Idx).Last - DTI.Dims (D_Idx).First + 1);
                                             else
                                                N_Tot := 0;
                                             end if;
                                          end loop;
                                          Emit_Inst (Ctx.E, R_T & " = const.i64 " & Img (Long_Long_Integer (N_Tot)));
                                          Tot64 := To_Unbounded_String (R_T);
                                       end;
                                    else
                                       declare
                                          Sp : constant String := Next_Reg (Ctx.E, "osp");
                                          Ln : constant String := Next_Reg (Ctx.E, "oln");
                                          T6 : constant String := Next_Reg (Ctx.E, "ot6");
                                       begin
                                          Emit_Inst (Ctx.E, Sp & " = sub.i32 " & To_String (Hi_Reg) & ", " & To_String (Lo_Reg));
                                          Emit_Inst (Ctx.E, Ln & " = add.i32 " & Sp & ", 1");
                                          Emit_Inst (Ctx.E, T6 & " = sext.i64 " & Ln);
                                          Tot64 := To_Unbounded_String (T6);
                                       end;
                                    end if;

                                    declare
                                       L_Hdr  : constant String := Next_Label (Ctx.E, "agg_hdr");
                                       L_Body : constant String := Next_Label (Ctx.E, "agg_bdy");
                                       L_Done : constant String := Next_Label (Ctx.E, "agg_end");
                                       I_Reg  : constant String := Next_Reg (Ctx.E, "ai");
                                       Cond   : constant String := Next_Reg (Ctx.E, "ac");
                                       EP     : constant String := Next_Reg (Ctx.E, "aep");
                                       Next_I : constant String := Next_Reg (Ctx.E, "ain");
                                    begin
                                       Emit_Term (Ctx.E, "jmp " & L_Hdr & "(0)");
                                       Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                                       Emit_Inst (Ctx.E, Cond & " = cmp_lt_s.i64 " & I_Reg & ", " & To_String (Tot64));
                                       Emit_Term (Ctx.E, "br " & Cond & ", " & L_Body & ", " & L_Done);
                                       Emit_Label (Ctx.E, L_Body);
                                       Emit_Inst (Ctx.E,
                                         EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & Dst_Ptr & ", " & I_Reg);
                                       if ETI.Kind = TK_Scalar then
                                          declare
                                             V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, SIt), DTI.Elem_Type);
                                          begin
                                             Emit_Inst (Ctx.E,
                                               "store." & To_String (ETI.Pir_Type) & " " & To_String (V.Reg) &
                                               ", " & EP & ", align " & Img (ETI.Align));
                                          end;
                                       else
                                          Emit_Init_Composite (Ctx, EP, DTI.Elem_Type, D (AS_EXP, SIt));
                                       end if;
                                       Emit_Inst (Ctx.E, Next_I & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                                       Emit_Term (Ctx.E, "jmp " & L_Hdr & "(" & Next_I & ")");
                                       Emit_Label (Ctx.E, L_Done);
                                    end;
                                 end;
                              end if;
                           end loop;
                        end;
                     end if;
                  end loop;
               end;

               -- Second pass: positional and named explicit elements
               while not IS_EMPTY (Assocs) loop
                  POP (Assocs, Item);
                  if KIND (Item) = DN_ASSOC or else KIND (Item) = DN_NAMED then
                     declare
                        CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, Item));
                        Ch : TREE;
                     begin
                        while not IS_EMPTY (CS) loop
                           POP (CS, Ch);
                           if KIND (Ch) = DN_CHOICE_EXP then
                              declare
                                 CE   : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Ch));
                                 C32  : constant String := Emit_Int_Convert (Ctx, To_String (CE.Reg), To_String (CE.Pir_Type), "i32");
                                 Off  : constant String := Next_Reg (Ctx.E, "aoff");
                                 O64  : constant String := Next_Reg (Ctx.E, "aoff64");
                                 EP   : constant String := Next_Reg (Ctx.E, "aep");
                              begin
                                 Emit_Inst (Ctx.E, Off & " = sub.i32 " & C32 & ", " & To_String (Lo_Reg));
                                 Emit_Inst (Ctx.E, O64 & " = sext.i64 " & Off);
                                 Emit_Inst (Ctx.E, EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & Dst_Ptr & ", " & O64);
                                 if ETI.Kind = TK_Scalar then
                                    declare
                                       V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Item), DTI.Elem_Type);
                                    begin
                                       Emit_Inst (Ctx.E,
                                         "store." & To_String (ETI.Pir_Type) & " " & To_String (V.Reg) &
                                         ", " & EP & ", align " & Img (ETI.Align));
                                    end;
                                 else
                                    Emit_Init_Composite (Ctx, EP, DTI.Elem_Type, D (AS_EXP, Item));
                                 end if;
                              end;
                           elsif KIND (Ch) = DN_CHOICE_RANGE then
                              declare
                                 RT, RL, RH : Unbounded_String;
                                 L_H : constant String := Next_Label (Ctx.E, "ar_h");
                                 L_B : constant String := Next_Label (Ctx.E, "ar_b");
                                 L_E : constant String := Next_Label (Ctx.E, "ar_e");
                                 IV  : constant String := Next_Reg (Ctx.E, "ari");
                                 Cnd : constant String := Next_Reg (Ctx.E, "arc");
                                 L32, H32 : Unbounded_String;
                              begin
                                 Emit_Discrete_Range (Ctx, D (AS_DISCRETE_RANGE, Ch), RT, RL, RH);
                                 L32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RL), To_String (RT), "i32"));
                                 H32 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RH), To_String (RT), "i32"));
                                 Emit_Term (Ctx.E, "jmp " & L_H & "(" & To_String (L32) & ")");
                                 Emit_Label (Ctx.E, L_H, IV & ": i32");
                                 Emit_Inst (Ctx.E, Cnd & " = cmp_le_s.i32 " & IV & ", " & To_String (H32));
                                 Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                                 Emit_Label (Ctx.E, L_B);
                                 declare
                                    Off : constant String := Next_Reg (Ctx.E, "aro");
                                    O64 : constant String := Next_Reg (Ctx.E, "aro64");
                                    EP  : constant String := Next_Reg (Ctx.E, "arep");
                                    Nx  : constant String := Next_Reg (Ctx.E, "arn");
                                 begin
                                    Emit_Inst (Ctx.E, Off & " = sub.i32 " & IV & ", " & To_String (Lo_Reg));
                                    Emit_Inst (Ctx.E, O64 & " = sext.i64 " & Off);
                                    Emit_Inst (Ctx.E, EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & Dst_Ptr & ", " & O64);
                                    if ETI.Kind = TK_Scalar then
                                       declare
                                          V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Item), DTI.Elem_Type);
                                       begin
                                          Emit_Inst (Ctx.E,
                                            "store." & To_String (ETI.Pir_Type) & " " & To_String (V.Reg) &
                                            ", " & EP & ", align " & Img (ETI.Align));
                                       end;
                                    else
                                       Emit_Init_Composite (Ctx, EP, DTI.Elem_Type, D (AS_EXP, Item));
                                    end if;
                                    Emit_Inst (Ctx.E, Nx & " = add.i32 " & IV & ", 1");
                                    Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                                 end;
                                 Emit_Label (Ctx.E, L_E);
                              end;
                           end if;
                        end loop;
                     end;
                  else
                     -- Positional element
                     declare
                        EP : constant String := Next_Reg (Ctx.E, "aep");
                     begin
                        Emit_Inst (Ctx.E,
                          EP & " = elem_ptr " & To_String (ETI.Pir_Type) & ", " & Dst_Ptr & ", " &
                          Img (Long_Long_Integer (Pos)));
                        if ETI.Kind = TK_Scalar then
                           declare
                              V : constant Expr_Val := Emit_Expr (Ctx, Item, DTI.Elem_Type);
                           begin
                              Emit_Inst (Ctx.E,
                                "store." & To_String (ETI.Pir_Type) & " " & To_String (V.Reg) &
                                ", " & EP & ", align " & Img (ETI.Align));
                           end;
                        else
                           Emit_Init_Composite (Ctx, EP, DTI.Elem_Type, Item);
                        end if;
                        Pos := Pos + 1;
                     end;
                  end if;
               end loop;
            end;
            return;
         end if;
      end if;

      -- General composite expression copy
      declare
         Src : constant Expr_Val := Emit_Expr (Ctx, Init_Node, Dst_Type);
      begin
         Emit_Copy_Composite (Ctx, Dst_Ptr, Src, Dst_Type);
      end;
   end Emit_Init_Composite;

   -- Emit a built-in operator call (from DN_BLTN_OPERATOR_ID)
   function Emit_Builtin_Op
     (Ctx        : in out Context;
      Op_Id      : TREE;
      Param_List : SEQ_TYPE;
      Res_Type   : TREE) return Expr_Val
   is
      Op_Pos : constant Integer := DI (SM_OPERATOR, Op_Id);
      -- Matches PRENAME.OP_CLASS order:
      -- 0:and, 1:or, 2:xor, 3:not, 4:u+, 5:u-, 6:abs,
      -- 7:=, 8:/=, 9:<, 10:<=, 11:>, 12:>=,
      -- 13:+, 14:-, 15:*, 16:/, 17:mod, 18:rem, 19:&, 20:**
      PL   : SEQ_TYPE := Param_List;
      A1_N : TREE := CONST_VOID;
      A2_N : TREE := CONST_VOID;
      Res  : Expr_Val;
      RTI  : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
   begin
      if not IS_EMPTY (PL) then
         POP (PL, A1_N);
      end if;
      if not IS_EMPTY (PL) then
         POP (PL, A2_N);
      end if;

      Res.Kind     := EK_Scalar;
      Res.Ada_Type := Res_Type;
      Res.Pir_Type := RTI.Pir_Type;
      Res.Align    := RTI.Align;

      -- Unary ops (3:not, 4:u+, 5:u-, 6:abs)
      if Op_Pos in 3 .. 6 then
         declare
            V  : constant Expr_Val := Emit_Expr (Ctx, A1_N, Res_Type);
            PT : constant String := To_String (V.Pir_Type);
            R  : constant String := Next_Reg (Ctx.E, "u");
         begin
            Res.Pir_Type := V.Pir_Type;
            if Op_Pos = 3 then -- NOT
               Emit_Inst (Ctx.E, R & " = xor." & PT & " " & To_String (V.Reg) & ", 1");
               Res.Reg := To_Unbounded_String (R);
            elsif Op_Pos = 4 then -- unary +
               Res.Reg := V.Reg;
            elsif Op_Pos = 5 then -- unary -
               if Is_Float_Type (Res_Type) then
                  Emit_Inst (Ctx.E, R & " = fneg." & PT & " " & To_String (V.Reg));
                  Res.Reg := To_Unbounded_String (R);
               else
                  if Ctx.Enable_Checks then
                     declare
                        Ovf : constant String := Next_Reg (Ctx.E, "ov");
                     begin
                        Emit_Inst (Ctx.E, R & ", " & Ovf & " = sub_ovf_s." & PT & " 0, " & To_String (V.Reg));
                        Emit_Check_Fail_If (Ctx, Ovf);
                     end;
                  else
                     Emit_Inst (Ctx.E, R & " = sub." & PT & " 0, " & To_String (V.Reg) & " [unsafe_s]");
                  end if;
                  Res.Reg := To_Unbounded_String (R);
               end if;
            elsif Op_Pos = 6 then -- ABS
               if Is_Float_Type (Res_Type) then
                  declare
                     Neg : constant String := Next_Reg (Ctx.E, "fn");
                     Cnd : constant String := Next_Reg (Ctx.E, "fc");
                  begin
                     Emit_Inst (Ctx.E, Neg & " = fneg." & PT & " " & To_String (V.Reg));
                     Emit_Inst (Ctx.E, Cnd & " = fcmp_olt." & PT & " " & To_String (V.Reg) & ", 0.0");
                     Emit_Inst (Ctx.E, R & " = select." & PT & " " & Cnd & ", " & Neg & ", " & To_String (V.Reg));
                     Res.Reg := To_Unbounded_String (R);
                  end;
               else
                  if Ctx.Enable_Checks then
                     declare
                        Is_Min : constant String := Next_Reg (Ctx.E, "amin");
                     begin
                        Emit_Inst (Ctx.E, Is_Min & " = cmp_eq." & PT & " " & To_String (V.Reg) & ", " &
                          Img (Min_Signed_For_Pir (PT)));
                        Emit_Check_Fail_If (Ctx, Is_Min);
                     end;
                  end if;
                  declare
                     Neg : constant String := Next_Reg (Ctx.E, "an");
                     Cnd : constant String := Next_Reg (Ctx.E, "ac");
                  begin
                     Emit_Inst (Ctx.E, Neg & " = sub." & PT & " 0, " & To_String (V.Reg));
                     Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s." & PT & " " & To_String (V.Reg) & ", 0");
                     Emit_Inst (Ctx.E, R & " = select." & PT & " " & Cnd & ", " & Neg & ", " & To_String (V.Reg));
                     Res.Reg := To_Unbounded_String (R);
                  end;
               end if;
            end if;
            return Res;
         end;
      end if;

      -- String/Array concatenation (& = 19)
      if Op_Pos = 19 then
         declare
            L   : constant Expr_Val := Emit_Expr (Ctx, A1_N);
            R_V : constant Expr_Val := Emit_Expr (Ctx, A2_N);
            BT  : constant TREE := Base_Type_Spec (Subst_Tree (Ctx, Res_Type));
            ETI : constant Type_Info := Get_Type_Info (Ctx, D (SM_COMP_TYPE, BT));
            ESz : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
            L_Len, R_Len : Unbounded_String;
            L_Ptr, R_Ptr : Unbounded_String;
            L_Is_Elem : constant Boolean := Is_Scalar_Type (L.Ada_Type) and then not Is_Array_Type (L.Ada_Type);
            R_Is_Elem : constant Boolean := Is_Scalar_Type (R_V.Ada_Type) and then not Is_Array_Type (R_V.Ada_Type);
         begin
            if L_Is_Elem then
               declare
                  Tmp : constant String := Next_Reg (Ctx.E, "ce");
                  One : constant String := Next_Reg (Ctx.E, "c1");
               begin
                  Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (ETI.Pir_Type) & ", align " & Img (ETI.Align));
                  Emit_Inst (Ctx.E, "store." & To_String (ETI.Pir_Type) & " " & To_String (L.Reg) & ", " & Tmp & ", align " & Img (ETI.Align));
                  Emit_Inst (Ctx.E, One & " = const.i32 1");
                  L_Ptr := To_Unbounded_String (Tmp);
                  L_Len := To_Unbounded_String (One);
               end;
            else
               declare
                  F, Hi : Unbounded_String;
                  Df : constant String := Next_Reg (Ctx.E, "cd");
                  Ln : constant String := Next_Reg (Ctx.E, "cl");
                  Ps : constant String := Next_Reg (Ctx.E, "cp");
                  Cl : constant String := Next_Reg (Ctx.E, "ccl");
               begin
                  Get_Array_Dim_Bounds (Ctx, L, 1, F, Hi);
                  Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (Hi) & ", " & To_String (F));
                  Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                  Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                  Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                  L_Ptr := L.Reg;
                  L_Len := To_Unbounded_String (Cl);
               end;
            end if;

            if R_Is_Elem then
               declare
                  Tmp : constant String := Next_Reg (Ctx.E, "ce");
                  One : constant String := Next_Reg (Ctx.E, "c1");
               begin
                  Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (ETI.Pir_Type) & ", align " & Img (ETI.Align));
                  Emit_Inst (Ctx.E, "store." & To_String (ETI.Pir_Type) & " " & To_String (R_V.Reg) & ", " & Tmp & ", align " & Img (ETI.Align));
                  Emit_Inst (Ctx.E, One & " = const.i32 1");
                  R_Ptr := To_Unbounded_String (Tmp);
                  R_Len := To_Unbounded_String (One);
               end;
            else
               declare
                  F, Hi : Unbounded_String;
                  Df : constant String := Next_Reg (Ctx.E, "cd");
                  Ln : constant String := Next_Reg (Ctx.E, "cl");
                  Ps : constant String := Next_Reg (Ctx.E, "cp");
                  Cl : constant String := Next_Reg (Ctx.E, "ccl");
               begin
                  Get_Array_Dim_Bounds (Ctx, R_V, 1, F, Hi);
                  Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (Hi) & ", " & To_String (F));
                  Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                  Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                  Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                  R_Ptr := R_V.Reg;
                  R_Len := To_Unbounded_String (Cl);
               end;
            end if;

            declare
               Tot    : constant String := Next_Reg (Ctx.E, "ctot");
               Tot64  : constant String := Next_Reg (Ctx.E, "ctot64");
               Bytes  : constant String := Next_Reg (Ctx.E, "cbytes");
               Buf_U  : Unbounded_String;
               L64    : constant String := Next_Reg (Ctx.E, "cl64");
               LBy    : constant String := Next_Reg (Ctx.E, "clby");
               R64    : constant String := Next_Reg (Ctx.E, "cr64");
               RBy    : constant String := Next_Reg (Ctx.E, "crby");
               Dst2   : constant String := Next_Reg (Ctx.E, "cdst2");
               One    : constant String := Next_Reg (Ctx.E, "cf1");
            begin
               Emit_Inst (Ctx.E, Tot & " = add.i32 " & To_String (L_Len) & ", " & To_String (R_Len));
               Emit_Inst (Ctx.E, Tot64 & " = sext.i64 " & Tot);
               Emit_Inst (Ctx.E, Bytes & " = mul.i64 " & Tot64 & ", " & Img (ESz));
               Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", Bytes & ", " & Img (ETI.Align), "ptr", Buf_U);
               Emit_Inst (Ctx.E, L64 & " = sext.i64 " & To_String (L_Len));
               Emit_Inst (Ctx.E, LBy & " = mul.i64 " & L64 & ", " & Img (ESz));
               Emit_Inst (Ctx.E, "memcpy " & To_String (Buf_U) & ", " & To_String (L_Ptr) & ", " & LBy & ", align " & Img (ETI.Align));
               Emit_Inst (Ctx.E, Dst2 & " = ptr_add.i64 " & To_String (Buf_U) & ", " & LBy & " [inbounds]");
               Emit_Inst (Ctx.E, R64 & " = sext.i64 " & To_String (R_Len));
               Emit_Inst (Ctx.E, RBy & " = mul.i64 " & R64 & ", " & Img (ESz));
               Emit_Inst (Ctx.E, "memcpy " & Dst2 & ", " & To_String (R_Ptr) & ", " & RBy & ", align " & Img (ETI.Align));
               Emit_Inst (Ctx.E, One & " = const.i32 1");
               Res.Kind           := EK_Slice;
               Res.Reg            := Buf_U;
               Res.Pir_Type       := To_Unbounded_String ("ptr");
               Res.Ada_Type       := BT;
               Res.Align          := ETI.Align;
               Res.NDims          := 1;
               Res.First_Regs (1) := To_Unbounded_String (One);
               Res.Last_Regs (1)  := To_Unbounded_String (Tot);
               return Res;
            end;
         end;
      end if;

      -- Binary ops
      declare
         L   : constant Expr_Val := Emit_Expr (Ctx, A1_N);
         R_V : constant Expr_Val := Emit_Expr (Ctx, A2_N);
         LT  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, L.Ada_Type));
         PT  : constant String := To_String (L.Pir_Type);
      begin
         -- Structural equality / inequality on records or arrays (Op_Pos 7:=, 8:/=)
         if (Op_Pos = 7 or else Op_Pos = 8) and then (L.Kind = EK_Addr or else L.Kind = EK_Slice) then
            declare
               LTI : constant Type_Info := Get_Type_Info (Ctx, LT);
               Eq_Res : Unbounded_String;
            begin
               if LTI.Kind = TK_Record or else (LTI.Kind = TK_Constrained_Array and then not LTI.Is_Dynamic) then
                  -- Byte-wise comparison loop over LTI.Byte_Size (works for flat arrays/records!)
                  declare
                     L_Hdr : constant String := Next_Label (Ctx.E, "eq_h");
                     L_Bdy : constant String := Next_Label (Ctx.E, "eq_b");
                     L_End : constant String := Next_Label (Ctx.E, "eq_e");
                     I_Reg : constant String := Next_Reg (Ctx.E, "eqi");
                     Done  : constant String := Next_Reg (Ctx.E, "eqd");
                     P1    : constant String := Next_Reg (Ctx.E, "eqp1");
                     P2    : constant String := Next_Reg (Ctx.E, "eqp2");
                     B1    : constant String := Next_Reg (Ctx.E, "eqb1");
                     B2    : constant String := Next_Reg (Ctx.E, "eqb2");
                     Neq   : constant String := Next_Reg (Ctx.E, "eqn");
                     Nx    : constant String := Next_Reg (Ctx.E, "eqnx");
                     OutR  : constant String := Next_Reg (Ctx.E, "eqr");
                  begin
                     Emit_Term (Ctx.E, "jmp " & L_Hdr & "(0)");
                     Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                     Emit_Inst (Ctx.E, Done & " = cmp_ge_s.i64 " & I_Reg & ", " & Img (LTI.Byte_Size));
                     Emit_Term (Ctx.E, "br " & Done & ", " & L_End & "(1), " & L_Bdy);
                     Emit_Label (Ctx.E, L_Bdy);
                     Emit_Inst (Ctx.E, P1 & " = ptr_add.i64 " & To_String (L.Reg) & ", " & I_Reg & " [inbounds]");
                     Emit_Inst (Ctx.E, P2 & " = ptr_add.i64 " & To_String (R_V.Reg) & ", " & I_Reg & " [inbounds]");
                     Emit_Inst (Ctx.E, B1 & " = load.i8 " & P1 & ", align 1");
                     Emit_Inst (Ctx.E, B2 & " = load.i8 " & P2 & ", align 1");
                     Emit_Inst (Ctx.E, Neq & " = cmp_ne.i8 " & B1 & ", " & B2);
                     Emit_Inst (Ctx.E, Nx & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                     Emit_Term (Ctx.E, "br " & Neq & ", " & L_End & "(0), " & L_Hdr & "(" & Nx & ")");
                     Emit_Label (Ctx.E, L_End, OutR & ": i8");
                     Eq_Res := To_Unbounded_String (OutR);
                  end;
               else
                  -- Dynamic/unconstrained 1D array equality: check lengths match, then byte-compare
                  declare
                     ETI : constant Type_Info := Get_Type_Info (Ctx, LTI.Elem_Type);
                     ESz : constant Natural := ((ETI.Byte_Size + ETI.Align - 1) / ETI.Align) * ETI.Align;
                     F1, L1, F2, L2 : Unbounded_String;
                     D1 : constant String := Next_Reg (Ctx.E, "d1");
                     N1 : constant String := Next_Reg (Ctx.E, "n1");
                     P1 : constant String := Next_Reg (Ctx.E, "p1");
                     C1 : constant String := Next_Reg (Ctx.E, "c1");
                     D2 : constant String := Next_Reg (Ctx.E, "d2");
                     N2 : constant String := Next_Reg (Ctx.E, "n2");
                     P2 : constant String := Next_Reg (Ctx.E, "p2");
                     C2 : constant String := Next_Reg (Ctx.E, "c2");
                     Leq : constant String := Next_Reg (Ctx.E, "leq");
                     N64 : constant String := Next_Reg (Ctx.E, "n64");
                     Byt : constant String := Next_Reg (Ctx.E, "byt");
                     L_Hdr : constant String := Next_Label (Ctx.E, "aeq_h");
                     L_Bdy : constant String := Next_Label (Ctx.E, "aeq_b");
                     L_End : constant String := Next_Label (Ctx.E, "aeq_e");
                     I_Reg : constant String := Next_Reg (Ctx.E, "ai");
                     Done  : constant String := Next_Reg (Ctx.E, "ad");
                     Q1    : constant String := Next_Reg (Ctx.E, "ap1");
                     Q2    : constant String := Next_Reg (Ctx.E, "ap2");
                     B1    : constant String := Next_Reg (Ctx.E, "ab1");
                     B2    : constant String := Next_Reg (Ctx.E, "ab2");
                     Neq   : constant String := Next_Reg (Ctx.E, "an");
                     Nx    : constant String := Next_Reg (Ctx.E, "anx");
                     OutR  : constant String := Next_Reg (Ctx.E, "ar");
                  begin
                     Get_Array_Dim_Bounds (Ctx, L, 1, F1, L1);
                     Get_Array_Dim_Bounds (Ctx, R_V, 1, F2, L2);
                     Emit_Inst (Ctx.E, D1 & " = sub.i32 " & To_String (L1) & ", " & To_String (F1));
                     Emit_Inst (Ctx.E, N1 & " = add.i32 " & D1 & ", 1");
                     Emit_Inst (Ctx.E, P1 & " = cmp_gt_s.i32 " & N1 & ", 0");
                     Emit_Inst (Ctx.E, C1 & " = select.i32 " & P1 & ", " & N1 & ", 0");
                     Emit_Inst (Ctx.E, D2 & " = sub.i32 " & To_String (L2) & ", " & To_String (F2));
                     Emit_Inst (Ctx.E, N2 & " = add.i32 " & D2 & ", 1");
                     Emit_Inst (Ctx.E, P2 & " = cmp_gt_s.i32 " & N2 & ", 0");
                     Emit_Inst (Ctx.E, C2 & " = select.i32 " & P2 & ", " & N2 & ", 0");
                     Emit_Inst (Ctx.E, Leq & " = cmp_eq.i32 " & C1 & ", " & C2);
                     Emit_Inst (Ctx.E, N64 & " = sext.i64 " & C1);
                     Emit_Inst (Ctx.E, Byt & " = mul.i64 " & N64 & ", " & Img (ESz));
                     Emit_Term (Ctx.E, "br " & Leq & ", " & L_Hdr & "(0), " & L_End & "(0)");
                     Emit_Label (Ctx.E, L_Hdr, I_Reg & ": i64");
                     Emit_Inst (Ctx.E, Done & " = cmp_ge_s.i64 " & I_Reg & ", " & Byt);
                     Emit_Term (Ctx.E, "br " & Done & ", " & L_End & "(1), " & L_Bdy);
                     Emit_Label (Ctx.E, L_Bdy);
                     Emit_Inst (Ctx.E, Q1 & " = ptr_add.i64 " & To_String (L.Reg) & ", " & I_Reg & " [inbounds]");
                     Emit_Inst (Ctx.E, Q2 & " = ptr_add.i64 " & To_String (R_V.Reg) & ", " & I_Reg & " [inbounds]");
                     Emit_Inst (Ctx.E, B1 & " = load.i8 " & Q1 & ", align 1");
                     Emit_Inst (Ctx.E, B2 & " = load.i8 " & Q2 & ", align 1");
                     Emit_Inst (Ctx.E, Neq & " = cmp_ne.i8 " & B1 & ", " & B2);
                     Emit_Inst (Ctx.E, Nx & " = add.i64 " & I_Reg & ", 1 [unsafe_s]");
                     Emit_Term (Ctx.E, "br " & Neq & ", " & L_End & "(0), " & L_Hdr & "(" & Nx & ")");
                     Emit_Label (Ctx.E, L_End, OutR & ": i8");
                     Eq_Res := To_Unbounded_String (OutR);
                  end;
               end if;

               if Op_Pos = 8 then
                  declare
                     Inv : constant String := Next_Reg (Ctx.E, "neq");
                  begin
                     Emit_Inst (Ctx.E, Inv & " = xor.i8 " & To_String (Eq_Res) & ", 1");
                     Res.Reg := To_Unbounded_String (Inv);
                  end;
               else
                  Res.Reg := Eq_Res;
               end if;
               Res.Pir_Type := To_Unbounded_String ("i8");
               return Res;
            end;
         end if;

         -- Coerce right operand to left operand's scalar PIR type when needed (except **)
         declare
            L_Reg : constant String := To_String (L.Reg);
            R_Reg : Unbounded_String := R_V.Reg;
            Out_R : constant String := Next_Reg (Ctx.E, "b");
         begin
            if Op_Pos /= 20 and then To_String (R_V.Pir_Type) /= PT then
               if Is_Float_Type (LT) then
                  declare
                     Cv : constant String := Next_Reg (Ctx.E, "fcv");
                  begin
                     if PT = "f64" and then To_String (R_V.Pir_Type) = "f32" then
                        Emit_Inst (Ctx.E, Cv & " = fpext.f64 " & To_String (R_Reg));
                     else
                        Emit_Inst (Ctx.E, Cv & " = fptrunc.f32 " & To_String (R_Reg));
                     end if;
                     R_Reg := To_Unbounded_String (Cv);
                  end;
               else
                  R_Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (R_Reg), To_String (R_V.Pir_Type), PT));
               end if;
            end if;

            case Op_Pos is
               when 0 => -- AND
                  Emit_Inst (Ctx.E, Out_R & " = and." & PT & " " & L_Reg & ", " & To_String (R_Reg));
               when 1 => -- OR
                  Emit_Inst (Ctx.E, Out_R & " = or." & PT & " " & L_Reg & ", " & To_String (R_Reg));
               when 2 => -- XOR
                  Emit_Inst (Ctx.E, Out_R & " = xor." & PT & " " & L_Reg & ", " & To_String (R_Reg));
               when 7 => -- =
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fcmp_oeq." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  else
                     Emit_Inst (Ctx.E, Out_R & " = cmp_eq." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  end if;
               when 8 => -- /=
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fcmp_une." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  else
                     Emit_Inst (Ctx.E, Out_R & " = cmp_ne." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  end if;
               when 9 => -- <
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fcmp_olt." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  else
                     Emit_Inst (Ctx.E, Out_R & " = cmp_lt_s." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  end if;
               when 10 => -- <=
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fcmp_ole." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  else
                     Emit_Inst (Ctx.E, Out_R & " = cmp_le_s." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  end if;
               when 11 => -- >
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fcmp_ogt." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  else
                     Emit_Inst (Ctx.E, Out_R & " = cmp_gt_s." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  end if;
               when 12 => -- >=
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fcmp_oge." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  else
                     Emit_Inst (Ctx.E, Out_R & " = cmp_ge_s." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  end if;
               when 13 => -- +
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fadd." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  elsif Ctx.Enable_Checks then
                     declare
                        Ovf : constant String := Next_Reg (Ctx.E, "ov");
                     begin
                        Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = add_ovf_s." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                        Emit_Check_Fail_If (Ctx, Ovf);
                     end;
                  else
                     Emit_Inst (Ctx.E, Out_R & " = add." & PT & " " & L_Reg & ", " & To_String (R_Reg) & " [unsafe_s]");
                  end if;
               when 14 => -- -
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fsub." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  elsif Ctx.Enable_Checks then
                     declare
                        Ovf : constant String := Next_Reg (Ctx.E, "ov");
                     begin
                        Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = sub_ovf_s." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                        Emit_Check_Fail_If (Ctx, Ovf);
                     end;
                  else
                     Emit_Inst (Ctx.E, Out_R & " = sub." & PT & " " & L_Reg & ", " & To_String (R_Reg) & " [unsafe_s]");
                  end if;
               when 15 => -- *
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fmul." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  elsif Ctx.Enable_Checks then
                     declare
                        Ovf : constant String := Next_Reg (Ctx.E, "ov");
                     begin
                        Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = mul_ovf_s." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                        Emit_Check_Fail_If (Ctx, Ovf);
                     end;
                  else
                     Emit_Inst (Ctx.E, Out_R & " = mul." & PT & " " & L_Reg & ", " & To_String (R_Reg) & " [unsafe_s]");
                  end if;
               when 16 => -- /
                  if Is_Float_Type (LT) then
                     Emit_Inst (Ctx.E, Out_R & " = fdiv." & PT & " " & L_Reg & ", " & To_String (R_Reg));
                  else
                     if Ctx.Enable_Checks then
                        declare
                           Z   : constant String := Next_Reg (Ctx.E, "dz");
                           Im  : constant String := Next_Reg (Ctx.E, "dm");
                           M1  : constant String := Next_Reg (Ctx.E, "dm1");
                           Ov  : constant String := Next_Reg (Ctx.E, "dov");
                           Bad : constant String := Next_Reg (Ctx.E, "dbad");
                        begin
                           Emit_Inst (Ctx.E, Z & " = cmp_eq." & PT & " " & To_String (R_Reg) & ", 0");
                           Emit_Inst (Ctx.E, Im & " = cmp_eq." & PT & " " & L_Reg & ", " & Img (Min_Signed_For_Pir (PT)));
                           Emit_Inst (Ctx.E, M1 & " = cmp_eq." & PT & " " & To_String (R_Reg) & ", -1");
                           Emit_Inst (Ctx.E, Ov & " = and.i8 " & Im & ", " & M1);
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & Z & ", " & Ov);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, Out_R & " = div_s." & PT & " " & L_Reg & ", " & To_String (R_Reg) & " [unsafe]");
                  end if;
               when 17 => -- MOD (Ada floor-mod: sign of right operand!)
                  if Ctx.Enable_Checks then
                     declare
                        Z : constant String := Next_Reg (Ctx.E, "mz");
                     begin
                        Emit_Inst (Ctx.E, Z & " = cmp_eq." & PT & " " & To_String (R_Reg) & ", 0");
                        Emit_Check_Fail_If (Ctx, Z);
                     end;
                  end if;
                  declare
                     RemR : constant String := Next_Reg (Ctx.E, "mr");
                     Nz   : constant String := Next_Reg (Ctx.E, "mnz");
                     RNeg : constant String := Next_Reg (Ctx.E, "mrn");
                     BNeg : constant String := Next_Reg (Ctx.E, "mbn");
                     Diff : constant String := Next_Reg (Ctx.E, "mdif");
                     Adj  : constant String := Next_Reg (Ctx.E, "madj");
                     Sum  : constant String := Next_Reg (Ctx.E, "msum");
                  begin
                     Emit_Inst (Ctx.E, RemR & " = rem_s." & PT & " " & L_Reg & ", " & To_String (R_Reg) & " [wrap]");
                     Emit_Inst (Ctx.E, Nz & " = cmp_ne." & PT & " " & RemR & ", 0");
                     Emit_Inst (Ctx.E, RNeg & " = cmp_lt_s." & PT & " " & RemR & ", 0");
                     Emit_Inst (Ctx.E, BNeg & " = cmp_lt_s." & PT & " " & To_String (R_Reg) & ", 0");
                     Emit_Inst (Ctx.E, Diff & " = xor.i8 " & RNeg & ", " & BNeg);
                     Emit_Inst (Ctx.E, Adj & " = and.i8 " & Nz & ", " & Diff);
                     Emit_Inst (Ctx.E, Sum & " = add." & PT & " " & RemR & ", " & To_String (R_Reg));
                     Emit_Inst (Ctx.E, Out_R & " = select." & PT & " " & Adj & ", " & Sum & ", " & RemR);
                  end;
               when 18 => -- REM (Ada rem: sign of left operand, matches PIR rem_s!)
                  if Ctx.Enable_Checks then
                     declare
                        Z : constant String := Next_Reg (Ctx.E, "rz");
                     begin
                        Emit_Inst (Ctx.E, Z & " = cmp_eq." & PT & " " & To_String (R_Reg) & ", 0");
                        Emit_Check_Fail_If (Ctx, Z);
                     end;
                  end if;
                  Emit_Inst (Ctx.E, Out_R & " = rem_s." & PT & " " & L_Reg & ", " & To_String (R_Reg) & " [wrap]");
               when 20 => -- ** (exponentiation)
                  -- Fast specialization for constant exponent 2 or 3
                  if not Is_Void_Tree (A2_N) and then Has_Static_Val (D (SM_VALUE, A2_N)) then
                     declare
                        EVal : constant Integer_64 := Eval_Int64 (D (SM_VALUE, A2_N));
                     begin
                        if EVal = 0 then
                           if Is_Float_Type (LT) then
                              Emit_Inst (Ctx.E, Out_R & " = const." & PT & " 1.0");
                           else
                              Emit_Inst (Ctx.E, Out_R & " = const." & PT & " 1");
                           end if;
                           Res.Reg := To_Unbounded_String (Out_R);
                           Res.Pir_Type := L.Pir_Type;
                           return Res;
                        elsif EVal = 1 then
                           Res.Reg := L.Reg;
                           Res.Pir_Type := L.Pir_Type;
                           return Res;
                        elsif EVal = 2 then
                           if Is_Float_Type (LT) then
                              Emit_Inst (Ctx.E, Out_R & " = fmul." & PT & " " & L_Reg & ", " & L_Reg);
                           elsif Ctx.Enable_Checks then
                              declare
                                 Ovf : constant String := Next_Reg (Ctx.E, "ov");
                              begin
                                 Emit_Inst (Ctx.E, Out_R & ", " & Ovf & " = mul_ovf_s." & PT & " " & L_Reg & ", " & L_Reg);
                                 Emit_Check_Fail_If (Ctx, Ovf);
                              end;
                           else
                              Emit_Inst (Ctx.E, Out_R & " = mul." & PT & " " & L_Reg & ", " & L_Reg & " [unsafe_s]");
                           end if;
                           Res.Reg := To_Unbounded_String (Out_R);
                           Res.Pir_Type := L.Pir_Type;
                           return Res;
                        end if;
                     end;
                  end if;
                  declare
                     E32 : constant String := Emit_Int_Convert (Ctx, To_String (R_V.Reg), To_String (R_V.Pir_Type), "i32");
                     Chk : constant String := (if Ctx.Enable_Checks then "1" else "0");
                     RV  : Unbounded_String;
                  begin
                     if Is_Float_Type (LT) then
                        if PT = "f32" then
                           Emit_Call_Or_Invoke (Ctx, "@__ada_fpow_f32", L_Reg & ", " & E32, "f32", RV, Nounwind => True);
                        else
                           Emit_Call_Or_Invoke (Ctx, "@__ada_fpow_f64", L_Reg & ", " & E32, "f64", RV, Nounwind => True);
                        end if;
                        Res.Reg := RV;
                        Res.Pir_Type := L.Pir_Type;
                        return Res;
                     else
                        if PT = "i64" then
                           Emit_Call_Or_Invoke (Ctx, "@__ada_ipow_i64", L_Reg & ", " & E32 & ", " & Chk, "i64", RV);
                           Res.Reg := RV;
                        else
                           declare
                              L32 : constant String := Emit_Int_Convert (Ctx, L_Reg, PT, "i32");
                           begin
                              Emit_Call_Or_Invoke (Ctx, "@__ada_ipow_i32", L32 & ", " & E32 & ", " & Chk, "i32", RV);
                              Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (RV), "i32", PT));
                           end;
                        end if;
                        Res.Pir_Type := L.Pir_Type;
                        return Res;
                     end if;
                  end;
               when others =>
                  Emit_Inst (Ctx.E, Out_R & " = const.i32 0");
            end case;

            Res.Reg := To_Unbounded_String (Out_R);
            if Op_Pos in 7 .. 12 then
               Res.Pir_Type := To_Unbounded_String ("i8");
            else
               Res.Pir_Type := L.Pir_Type;
            end if;
            return Res;
         end;
      end;
   end Emit_Builtin_Op;

   -- Handle predefined RTL calls (Text_IO, Calendar, Unchecked_Deallocation, Unchecked_Conversion)
   function Try_Emit_RTL_Call
     (Ctx        : in out Context;
      Def        : TREE;
      Param_List : SEQ_TYPE;
      Res_Type   : TREE;
      Handled    : out Boolean) return Expr_Val
   is
      Pkg  : constant String := Enclosing_Package_Name (Def);
      Name : constant String := Sym_Name (Def);
      Res  : Expr_Val;
      PL   : SEQ_TYPE := Param_List;
      Args : array (1 .. 8) of TREE := (others => CONST_VOID);
      NArg : Natural := 0;
   begin
      Handled := False;
      while not IS_EMPTY (PL) and then NArg < 8 loop
         NArg := NArg + 1;
         POP (PL, Args (NArg));
      end loop;

      -- Check for generic instantiation of UNCHECKED_DEALLOCATION / UNCHECKED_CONVERSION
      if KIND (Def) in CLASS_SUBPROG_NAME then
         declare
            UD : constant TREE := D (SM_UNIT_DESC, Def);
         begin
            if not Is_Void_Tree (UD) and then KIND (UD) = DN_INSTANTIATION then
               declare
                  Gen_Def : constant TREE := Get_Name_Defn (D (AS_NAME, UD));
                  Gen_N   : constant String := Sym_Name (Gen_Def);
               begin
                  if Gen_N = "UNCHECKED_DEALLOCATION" then
                     Handled := True;
                     declare
                        LV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                        PV : constant String := Next_Reg (Ctx.E, "fp");
                        Dummy : Unbounded_String;
                     begin
                        Emit_Inst (Ctx.E, PV & " = load.ptr " & To_String (LV.Reg) & ", align 8");
                        Emit_Call_Or_Invoke (Ctx, "@__ada_free", PV, "", Dummy, Nounwind => True);
                        Emit_Inst (Ctx.E, "store.ptr 0, " & To_String (LV.Reg) & ", align 8");
                     end;
                     return Res;
                  elsif Gen_N = "UNCHECKED_CONVERSION" then
                     Handled := True;
                     declare
                        Src : constant Expr_Val := Emit_Expr (Ctx, Args (1));
                        RTI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
                        R   : constant String := Next_Reg (Ctx.E, "uc");
                     begin
                        if To_String (Src.Pir_Type) = To_String (RTI.Pir_Type) then
                           Res := Src;
                           Res.Ada_Type := Res_Type;
                        elsif To_String (Src.Pir_Type) = "ptr" then
                           Emit_Inst (Ctx.E, R & " = ptrtoint." & To_String (RTI.Pir_Type) & " " & To_String (Src.Reg));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                        elsif To_String (RTI.Pir_Type) = "ptr" then
                           Emit_Inst (Ctx.E, R & " = inttoptr.ptr " & To_String (Src.Reg));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                        else
                           Emit_Inst (Ctx.E, R & " = bitcast." & To_String (RTI.Pir_Type) & " " & To_String (Src.Reg));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                        end if;
                        return Res;
                     end;
                  end if;
               end;
            end if;
         end;
      end if;

      -- Check if enclosing package is an instantiation of TEXT_IO.INTEGER_IO / FLOAT_IO / FIXED_IO
      declare
         Reg : constant TREE := (if KIND (Def) in CLASS_SOURCE_NAME then D (XD_REGION, Def) else CONST_VOID);
      begin
         if not Is_Void_Tree (Reg) and then KIND (Reg) = DN_PACKAGE_ID then
            declare
               PUD : constant TREE := D (SM_UNIT_DESC, Reg);
            begin
               if not Is_Void_Tree (PUD) and then KIND (PUD) = DN_INSTANTIATION then
                  declare
                     Gen_P : constant TREE := Get_Name_Defn (D (AS_NAME, PUD));
                     GP_N  : constant String := Sym_Name (Gen_P);
                  begin
                     if GP_N = "INTEGER_IO" and then Name = "PUT" then
                        Handled := True;
                        -- PUT(ITEM, WIDTH, BASE) -> 3 args (or 4 with FILE)
                        declare
                           Off : constant Natural := (if NArg = 4 then 1 else 0);
                           IV  : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
                           WV  : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                           BV  : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                           I64 : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i64");
                           W32 : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                           B32 : constant String := Emit_Int_Convert (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), "i32");
                           Dummy : Unbounded_String;
                        begin
                           Emit_Call_Or_Invoke (Ctx, "@__ada_put_i64", I64 & ", " & W32 & ", " & B32, "", Dummy, Nounwind => True);
                        end;
                        return Res;
                     elsif (GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO") and then Name = "PUT" then
                        Handled := True;
                        declare
                           Off : constant Natural := (if NArg = 5 then 1 else 0);
                           FV  : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
                           FrV : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                           AfV : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                           ExV : constant Expr_Val := Emit_Expr (Ctx, Args (4 + Off));
                           F64 : Unbounded_String := FV.Reg;
                           Fr32 : constant String := Emit_Int_Convert (Ctx, To_String (FrV.Reg), To_String (FrV.Pir_Type), "i32");
                           Af32 : constant String := Emit_Int_Convert (Ctx, To_String (AfV.Reg), To_String (AfV.Pir_Type), "i32");
                           Ex32 : constant String := Emit_Int_Convert (Ctx, To_String (ExV.Reg), To_String (ExV.Pir_Type), "i32");
                           Dummy : Unbounded_String;
                        begin
                           if GP_N = "FIXED_IO" then
                              declare
                                 Fl  : constant String := Next_Reg (Ctx.E, "fxf");
                                 Sc  : constant String := Next_Reg (Ctx.E, "fxs");
                                 Sm  : constant IEEE_Float_64 := Fixed_Small (FV.Ada_Type);
                              begin
                                 Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (FV.Reg));
                                 Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Sm));
                                 F64 := To_Unbounded_String (Sc);
                              end;
                           elsif To_String (FV.Pir_Type) = "f32" then
                              declare
                                 Ext : constant String := Next_Reg (Ctx.E, "fe");
                              begin
                                 Emit_Inst (Ctx.E, Ext & " = fpext.f64 " & To_String (FV.Reg));
                                 F64 := To_Unbounded_String (Ext);
                              end;
                           end if;
                           Emit_Call_Or_Invoke (Ctx, "@__ada_put_f64",
                             To_String (F64) & ", " & Fr32 & ", " & Af32 & ", " & Ex32, "", Dummy, Nounwind => True);
                        end;
                        return Res;
                     end if;
                  end;
               end if;
            end;
         end if;
      end;

      if Pkg = "TEXT_IO" then
         if Name = "NEW_LINE" then
            Handled := True;
            declare
               Off : constant Natural := (if NArg = 2 then 1 else 0);
               Sp  : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
               S32 : constant String := Emit_Int_Convert (Ctx, To_String (Sp.Reg), To_String (Sp.Pir_Type), "i32");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_new_line", S32, "", Dummy, Nounwind => True);
            end;
            return Res;
         elsif Name = "PUT" or else Name = "PUT_LINE" then
            Handled := True;
            declare
               Off : constant Natural := (if NArg = 2 then 1 else 0);
               V   : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
               Dummy : Unbounded_String;
            begin
               if Is_Character_Type (V.Ada_Type) then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_put_char", To_String (V.Reg), "", Dummy, Nounwind => True);
               else
                  declare
                     F, L : Unbounded_String;
                     Df : constant String := Next_Reg (Ctx.E, "sd");
                     Ln : constant String := Next_Reg (Ctx.E, "sl");
                     Ps : constant String := Next_Reg (Ctx.E, "sp");
                     Cl : constant String := Next_Reg (Ctx.E, "scl");
                     Fn : constant String := (if Name = "PUT_LINE" then "@__ada_put_line" else "@__ada_put_str");
                  begin
                     Get_Array_Dim_Bounds (Ctx, V, 1, F, L);
                     Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L) & ", " & To_String (F));
                     Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                     Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                     Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                     Emit_Call_Or_Invoke (Ctx, Fn, To_String (V.Reg) & ", " & Cl, "", Dummy, Nounwind => True);
                  end;
               end if;
            end;
            return Res;
         end if;
      elsif Pkg = "CALENDAR" then
         if Name = "CLOCK" then
            Handled := True;
            declare
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_clock_ns", "", "i64", RV, Nounwind => True);
               Res.Kind     := EK_Scalar;
               Res.Reg      := RV;
               Res.Pir_Type := To_Unbounded_String ("i64");
               Res.Ada_Type := Res_Type;
               Res.Align    := 8;
            end;
            return Res;
         elsif Name = """-""" or else Name = """+""" or else Name = """<"""
           or else Name = """<=""" or else Name = """>""" or else Name = """>="""
         then
            Handled := True;
            declare
               L  : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               R_V : constant Expr_Val := Emit_Expr (Ctx, Args (2));
               Out_R : constant String := Next_Reg (Ctx.E, "cal");
            begin
               Res.Kind := EK_Scalar;
               Res.Ada_Type := Res_Type;
               if Name = """-""" then
                  Emit_Inst (Ctx.E, Out_R & " = sub.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                  Res.Pir_Type := To_Unbounded_String ("i64");
               elsif Name = """+""" then
                  Emit_Inst (Ctx.E, Out_R & " = add.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                  Res.Pir_Type := To_Unbounded_String ("i64");
               elsif Name = """<""" then
                  Emit_Inst (Ctx.E, Out_R & " = cmp_lt_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                  Res.Pir_Type := To_Unbounded_String ("i8");
               elsif Name = """<=""" then
                  Emit_Inst (Ctx.E, Out_R & " = cmp_le_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                  Res.Pir_Type := To_Unbounded_String ("i8");
               elsif Name = """>""" then
                  Emit_Inst (Ctx.E, Out_R & " = cmp_gt_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                  Res.Pir_Type := To_Unbounded_String ("i8");
               else
                  Emit_Inst (Ctx.E, Out_R & " = cmp_ge_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                  Res.Pir_Type := To_Unbounded_String ("i8");
               end if;
               Res.Reg := To_Unbounded_String (Out_R);
               return Res;
            end;
         end if;
      end if;
      return Res;
   end Try_Emit_RTL_Call;

   -- Compute the static link pointer to pass when calling Callee_SI from Ctx.Cur_Subp_Key
   function Compute_Call_SL (Ctx : in out Context; Callee_SI : Subp_Info) return String is
   begin
      if Callee_SI.Parent_Subp = Ctx.Cur_Subp_Key then
         return To_String (Ctx.Cur_Frame_Reg);
      end if;
      declare
         Cur_K : Unsigned_32 := Ctx.Cur_Subp_Key;
         SL    : Unbounded_String := Ctx.Cur_SL_Reg;
      begin
         for Hop in 1 .. 16 loop
            exit when not Ctx.Subps.Contains (Cur_K);
            declare
               SI : constant Subp_Info := Ctx.Subps.Element (Cur_K);
               Parent_K : constant Unsigned_32 := SI.Parent_Subp;
            begin
               if Parent_K = Callee_SI.Parent_Subp then
                  return To_String (SL);
               elsif Parent_K = 0 then
                  return "0";
               else
                  declare
                     PI  : constant Subp_Info := Ctx.Subps.Element (Parent_K);
                     FP  : constant String := Next_Reg (Ctx.E, "cslp");
                     NSL : constant String := Next_Reg (Ctx.E, "csl");
                  begin
                     Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (PI.Frame_Type) & ", " & To_String (SL) & ", 0");
                     Emit_Inst (Ctx.E, NSL & " = load.ptr " & FP & ", align 8");
                     SL := To_Unbounded_String (NSL);
                     Cur_K := Parent_K;
                  end;
               end if;
            end;
         end loop;
      end;
      return "0";
   end Compute_Call_SL;

   function Emit_Subp_Call
     (Ctx       : in out Context;
      Call_Node : TREE;
      Is_Func   : Boolean) return Expr_Val
   is
      Name_Node : constant TREE := D (AS_NAME, Call_Node);
      Des_Node  : TREE := Name_Node;
      Def       : TREE;
      Norm_S    : TREE;
      Param_L   : SEQ_TYPE := MAKE;
      Res_Type  : TREE := CONST_VOID;
      Res       : Expr_Val;
   begin
      if KIND (Des_Node) = DN_SELECTED then
         Des_Node := D (AS_DESIGNATOR, Des_Node);
      end if;
      if Is_Func then
         Res_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Call_Node));
      end if;
      if KIND (Call_Node) /= DN_NULLARY_CALL then
         Norm_S := D (SM_NORMALIZED_PARAM_S, Call_Node);
         if not Is_Void_Tree (Norm_S) then
            Param_L := LIST (Norm_S);
         else
            declare
               GA : constant TREE := D (AS_GENERAL_ASSOC_S, Call_Node);
            begin
               if not Is_Void_Tree (GA) then
                  Param_L := LIST (GA);
               end if;
            end;
         end if;
      end if;
      if KIND (Des_Node) = DN_ATTRIBUTE then
         -- Attribute function call like Color'Succ(C), Color'Pos(Yellow), Color'Val(2)
         declare
            Attr_Id : constant TREE := D (AS_USED_NAME_ID, Des_Node);
            AName   : constant String := Sym_Name (Attr_Id);
            ET      : constant TREE := Res_Type;
            TI      : constant Type_Info := Get_Type_Info (Ctx, ET);
            Arg_N   : TREE := CONST_VOID;
            PL      : SEQ_TYPE := Param_L;
         begin
            if not IS_EMPTY (PL) then
               POP (PL, Arg_N);
            end if;
            if AName = "POS" then
               declare
                  V : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
               begin
                  Res.Kind := EK_Scalar; Res.Ada_Type := ET; Res.Pir_Type := TI.Pir_Type; Res.Align := TI.Align;
                  Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type)));
                  return Res;
               end;
            elsif AName = "VAL" then
               declare
                  V  : constant Expr_Val := Emit_Expr (Ctx, Arg_N);
                  Cv : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type));
               begin
                  Emit_Range_Check (Ctx, Cv, To_String (TI.Pir_Type), ET);
                  Res.Kind := EK_Scalar; Res.Ada_Type := ET; Res.Pir_Type := TI.Pir_Type; Res.Align := TI.Align;
                  Res.Reg := To_Unbounded_String (Cv);
                  return Res;
               end;
            elsif AName = "SUCC" or else AName = "PRED" then
               declare
                  V    : constant Expr_Val := Emit_Expr (Ctx, Arg_N, ET);
                  PT   : constant String := To_String (TI.Pir_Type);
                  OutR : constant String := Next_Reg (Ctx.E, "sp");
                  Op   : constant String := (if AName = "SUCC" then "add" else "sub");
               begin
                  if Ctx.Enable_Checks then
                     declare
                        Ovf : constant String := Next_Reg (Ctx.E, "ov");
                     begin
                        Emit_Inst (Ctx.E, OutR & ", " & Ovf & " = " & Op & "_ovf_s." & PT & " " & To_String (V.Reg) & ", 1");
                        Emit_Check_Fail_If (Ctx, Ovf);
                     end;
                     Emit_Range_Check (Ctx, OutR, PT, ET);
                  else
                     Emit_Inst (Ctx.E, OutR & " = " & Op & "." & PT & " " & To_String (V.Reg) & ", 1");
                  end if;
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (OutR);
                  Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                  return Res;
               end;
            end if;
         end;
      end if;
      Def := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Des_Node)));

      -- 1. Built-in operator
      if KIND (Def) = DN_BLTN_OPERATOR_ID then
         return Emit_Builtin_Op (Ctx, Def, Param_L, Res_Type);
      end if;

      -- 2. Implicit /= on user-defined "="
      if KIND (Def) in CLASS_SUBPROG_NAME then
         declare
            UD : constant TREE := D (SM_UNIT_DESC, Def);
         begin
            if not Is_Void_Tree (UD) and then KIND (UD) = DN_IMPLICIT_NOT_EQ then
               -- Call the underlying "=" and invert
               declare
                  Eq_Def : constant TREE := Canonical_Def (D (SM_EQUAL, UD));
                  Eq_Call : Expr_Val;
                  Inv     : constant String := Next_Reg (Ctx.E, "ineq");
                  -- Temporarily invoke Eq_Def via direct call helper
                  SI      : Subp_Info;
                  Args    : Unbounded_String;
                  RV      : Unbounded_String;
               begin
                  if Ctx.Subps.Contains (Tree_Key (Eq_Def)) then
                     SI := Ctx.Subps.Element (Tree_Key (Eq_Def));
                     declare
                        PL : SEQ_TYPE := Param_L;
                        A1, A2 : TREE;
                        V1, V2 : Expr_Val;
                     begin
                        POP (PL, A1); POP (PL, A2);
                        V1 := Emit_Expr (Ctx, A1); V2 := Emit_Expr (Ctx, A2);
                        if SI.Needs_SL then
                           Append (Args, Compute_Call_SL (Ctx, SI) & ", ");
                        end if;
                        Append (Args, To_String (V1.Reg) & ", " & To_String (V2.Reg));
                        Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args), "i8", RV);
                        Emit_Inst (Ctx.E, Inv & " = xor.i8 " & To_String (RV) & ", 1");
                        Eq_Call.Kind := EK_Scalar;
                        Eq_Call.Reg := To_Unbounded_String (Inv);
                        Eq_Call.Pir_Type := To_Unbounded_String ("i8");
                        Eq_Call.Ada_Type := Res_Type;
                        return Eq_Call;
                     end;
                  end if;
               end;
            end if;
         end;
      end if;

      -- 3. Predefined RTL calls (Text_IO, Calendar, Unchecked_*)
      declare
         RTL_Handled : Boolean := False;
      begin
         Res := Try_Emit_RTL_Call (Ctx, Def, Param_L, Res_Type, RTL_Handled);
         if RTL_Handled then
            return Res;
         end if;
      end;

      -- 4. User subprogram call (local, nested, generic instance, or external package)
      declare
         Key : constant Unsigned_32 := Tree_Key (Def);
         SI  : Subp_Info;
      begin
         if Ctx.Subps.Contains (Key) then
            SI := Ctx.Subps.Element (Key);
         else
            -- External subprogram from a with'd package spec
            declare
               Spec : constant TREE := D (SM_SPEC, Def);
            begin
               SI.Spec_Node   := Spec;
               SI.Pir_Name    := To_Unbounded_String (Mangle_Subp_Name (Ctx, Def, Spec));
               SI.Is_Function := KIND (Spec) = DN_FUNCTION_SPEC;
               SI.Is_External := True;
               if SI.Is_Function then
                  SI.Ret_Type := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, Spec))));
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  begin
                     SI.Ret_Composite := RTI.Kind /= TK_Scalar;
                     SI.Ret_Uncon_Arr := RTI.Kind = TK_Unconstrained_Array
                       or else (RTI.Kind = TK_Constrained_Array and then RTI.Is_Dynamic);
                  end;
               end if;
               Ctx.Subps.Insert (Key, SI);
               Ensure_Extern_Subp (Ctx, Def, SI);
            end;
         end if;

         declare
            Args_Buf : Unbounded_String;
            First_A  : Boolean := True;
            Ret_Buf  : Unbounded_String;

            procedure Add_Arg (S : String) is
            begin
               if not First_A then
                  Append (Args_Buf, ", ");
               end if;
               First_A := False;
               Append (Args_Buf, S);
            end Add_Arg;
         begin
            if SI.Needs_SL then
               Add_Arg (Compute_Call_SL (Ctx, SI));
            end if;
            if SI.Ret_Uncon_Arr then
               declare
                  RTI     : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, RTI.NDims);
                  Tmp     : constant String := Next_Reg (Ctx.E, "retdesc");
               begin
                  Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & Desc_Ty & ", align 8");
                  Ret_Buf := To_Unbounded_String (Tmp);
                  Add_Arg (Tmp);
               end;
            elsif SI.Ret_Composite then
               declare
                  RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  Tmp : constant String := Next_Reg (Ctx.E, "retbuf");
               begin
                  Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (RTI.Pir_Type) & ", align " & Img (RTI.Align));
                  Ret_Buf := To_Unbounded_String (Tmp);
                  Add_Arg (Tmp);
               end;
            end if;

            -- Walk formal parameters in lockstep with actuals
            if not Is_Void_Tree (SI.Spec_Node) then
               declare
                  PS : constant TREE := D (AS_PARAM_S, SI.Spec_Node);
                  FL : SEQ_TYPE := (if Is_Void_Tree (PS) then MAKE else LIST (PS));
                  AL : SEQ_TYPE := Param_L;
                  FP : TREE;
               begin
                  while not IS_EMPTY (FL) loop
                     POP (FL, FP);
                     declare
                        Mode : constant NODE_NAME := KIND (FP);
                        FNS  : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, FP));
                        FID  : TREE;
                        Act  : TREE;
                     begin
                        while not IS_EMPTY (FNS) loop
                           POP (FNS, FID);
                           if not IS_EMPTY (AL) then
                              POP (AL, Act);
                           else
                              Act := D (SM_INIT_EXP, FID);
                           end if;
                           declare
                              FT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, FID));
                              FTI : constant Type_Info := Get_Type_Info (Ctx, FT);
                           begin
                              if FTI.Kind = TK_Unconstrained_Array then
                                 declare
                                    AV : Expr_Val;
                                 begin
                                    if Mode = DN_IN then
                                       AV := Emit_Expr (Ctx, Act, FT);
                                    else
                                       AV := Emit_LValue (Ctx, Act);
                                    end if;
                                    Add_Arg (To_String (AV.Reg));
                                    for D_Idx in 1 .. FTI.NDims loop
                                       declare
                                          F_R, L_R : Unbounded_String;
                                       begin
                                          Get_Array_Dim_Bounds (Ctx, AV, D_Idx, F_R, L_R);
                                          Add_Arg (To_String (F_R));
                                          Add_Arg (To_String (L_R));
                                       end;
                                    end loop;
                                 end;
                              elsif FTI.Kind = TK_Record or else FTI.Kind = TK_Constrained_Array then
                                 declare
                                    AV : Expr_Val;
                                 begin
                                    if Mode = DN_IN then
                                       AV := Emit_Expr (Ctx, Act, FT);
                                    else
                                       AV := Emit_LValue (Ctx, Act);
                                    end if;
                                    Add_Arg (To_String (AV.Reg));
                                 end;
                              elsif Mode = DN_OUT or else Mode = DN_IN_OUT then
                                 declare
                                    LV : constant Expr_Val := Emit_LValue (Ctx, Act);
                                 begin
                                    Add_Arg (To_String (LV.Reg));
                                 end;
                              else
                                 declare
                                    AV : constant Expr_Val := Emit_Expr (Ctx, Act, FT);
                                    Reg_S : Unbounded_String := AV.Reg;
                                 begin
                                    if To_String (AV.Pir_Type) /= To_String (FTI.Pir_Type) and then not Is_Float_Type (FT) then
                                       Reg_S := To_Unbounded_String (
                                         Emit_Int_Convert (Ctx, To_String (AV.Reg), To_String (AV.Pir_Type), To_String (FTI.Pir_Type)));
                                    end if;
                                    Emit_Range_Check (Ctx, To_String (Reg_S), To_String (FTI.Pir_Type), FT);
                                    Add_Arg (To_String (Reg_S));
                                 end;
                              end if;
                           end;
                        end loop;
                     end;
                  end loop;
               end;
            end if;

            declare
               RV : Unbounded_String;
            begin
               if not SI.Is_Function or else SI.Ret_Composite then
                  Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args_Buf), "", RV);
                  if SI.Ret_Uncon_Arr then
                     declare
                        RTI     : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                        ETI     : constant Type_Info := Get_Type_Info (Ctx, RTI.Elem_Type);
                        ND      : constant Natural := (if RTI.NDims > 0 then RTI.NDims else 1);
                        Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, ND);
                        P_Fld   : constant String := Next_Reg (Ctx.E, "rdpf");
                        D_Ptr   : constant String := Next_Reg (Ctx.E, "rdp");
                     begin
                        Emit_Inst (Ctx.E, P_Fld & " = field_ptr " & Desc_Ty & ", " & To_String (Ret_Buf) & ", 0");
                        Emit_Inst (Ctx.E, D_Ptr & " = load.ptr " & P_Fld & ", align 8");
                        Res.Kind     := EK_Slice;
                        Res.Reg      := To_Unbounded_String (D_Ptr);
                        Res.Pir_Type := To_Unbounded_String ("ptr");
                        Res.Ada_Type := SI.Ret_Type;
                        Res.Align    := ETI.Align;
                        Res.NDims    := ND;
                        for D_Idx in 1 .. ND loop
                           declare
                              FF : constant String := Next_Reg (Ctx.E, "rdff");
                              FV : constant String := Next_Reg (Ctx.E, "rdf");
                              LF : constant String := Next_Reg (Ctx.E, "rdlf");
                              LV : constant String := Next_Reg (Ctx.E, "rdl");
                           begin
                              Emit_Inst (Ctx.E, FF & " = field_ptr " & Desc_Ty & ", " & To_String (Ret_Buf) & ", " & Img (2 * D_Idx - 1));
                              Emit_Inst (Ctx.E, FV & " = load.i32 " & FF & ", align 4");
                              Emit_Inst (Ctx.E, LF & " = field_ptr " & Desc_Ty & ", " & To_String (Ret_Buf) & ", " & Img (2 * D_Idx));
                              Emit_Inst (Ctx.E, LV & " = load.i32 " & LF & ", align 4");
                              Res.First_Regs (D_Idx) := To_Unbounded_String (FV);
                              Res.Last_Regs (D_Idx)  := To_Unbounded_String (LV);
                           end;
                        end loop;
                     end;
                  elsif SI.Ret_Composite then
                     declare
                        RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                     begin
                        Res.Kind     := EK_Addr;
                        Res.Reg      := Ret_Buf;
                        Res.Pir_Type := RTI.Pir_Type;
                        Res.Ada_Type := SI.Ret_Type;
                        Res.Align    := RTI.Align;
                     end;
                  end if;
               else
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  begin
                     Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args_Buf), To_String (RTI.Pir_Type), RV);
                     Res.Kind     := EK_Scalar;
                     Res.Reg      := RV;
                     Res.Pir_Type := RTI.Pir_Type;
                     Res.Ada_Type := SI.Ret_Type;
                     Res.Align    := RTI.Align;
                  end;
               end if;
            end;
            return Res;
         end;
      end;
   end Emit_Subp_Call;

   function Emit_Attribute (Ctx : in out Context; Node : TREE; Target_Type : TREE) return Expr_Val is
      Attr_Id : constant TREE := D (AS_USED_NAME_ID, Node);
      AName   : constant String := Sym_Name (Attr_Id);
      Pfx     : constant TREE := D (AS_NAME, Node);
      Arg     : constant TREE := D (AS_EXP, Node);
      ET      : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
      Res     : Expr_Val;
   begin
      if Is_Void_Tree (ET) or else KIND (ET) = DN_UNIVERSAL_INTEGER then
         if not Is_Void_Tree (Target_Type) and then Is_Scalar_Type (Target_Type) then
            ET := Target_Type;
         end if;
      end if;
      declare
         TI : constant Type_Info := Get_Type_Info (Ctx, ET);
      begin
         -- Static value already folded by semphase!
         if Has_Static_Val (D (SM_VALUE, Node)) then
            declare
               SV : constant TREE := D (SM_VALUE, Node);
               R  : constant String := Next_Reg (Ctx.E, "ac");
            begin
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := TI.Pir_Type;
               Res.Align    := TI.Align;
               if Is_Float_Type (ET) then
                  Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (Eval_Float64 (SV)));
               else
                  Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Eval_Int64 (SV))));
               end if;
               Res.Reg := To_Unbounded_String (R);
               return Res;
            end;
         end if;

         -- Array attributes: FIRST, LAST, LENGTH
         if AName = "FIRST" or else AName = "LAST" or else AName = "LENGTH" then
            declare
               Dim : Natural := 1;
            begin
               if not Is_Void_Tree (Arg) and then Has_Static_Val (D (SM_VALUE, Arg)) then
                  Dim := Natural (Eval_Int64 (D (SM_VALUE, Arg)));
               end if;
               -- Check if prefix is a scalar type mark
               if KIND (Pfx) = DN_USED_NAME_ID and then KIND (D (SM_DEFN, Pfx)) in CLASS_TYPE_NAME then
                  declare
                     TS : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (SM_DEFN, Pfx)));
                     St : Boolean;
                     Lo, Hi : Integer_64;
                  begin
                     if Is_Discrete_Type (TS) then
                        Get_Discrete_Bounds (TS, St, Lo, Hi);
                        declare
                           Val : constant Integer_64 := (if AName = "FIRST" then Lo else Hi);
                           R   : constant String := Next_Reg (Ctx.E, "tb");
                        begin
                           Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Val)));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     elsif Is_Constrained_Array (TS) then
                        declare
                           ATI : constant Type_Info := Get_Type_Info (Ctx, TS);
                           LoV : constant Integer_64 := ATI.Dims (Dim).First;
                           HiV : constant Integer_64 := ATI.Dims (Dim).Last;
                           Val : Integer_64;
                           R   : constant String := Next_Reg (Ctx.E, "ab");
                        begin
                           if AName = "FIRST" then Val := LoV;
                           elsif AName = "LAST" then Val := HiV;
                           else Val := (if HiV >= LoV then HiV - LoV + 1 else 0);
                           end if;
                           Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Val)));
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     end if;
                  end;
               end if;

               -- Array object prefix
               declare
                  AV : constant Expr_Val := Emit_LValue (Ctx, Pfx);
                  F_R, L_R : Unbounded_String;
               begin
                  Get_Array_Dim_Bounds (Ctx, AV, Dim, F_R, L_R);
                  Res.Kind     := EK_Scalar;
                  Res.Ada_Type := ET;
                  Res.Pir_Type := TI.Pir_Type;
                  Res.Align    := TI.Align;
                  if AName = "FIRST" then
                     Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (F_R), "i32", To_String (TI.Pir_Type)));
                  elsif AName = "LAST" then
                     Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (L_R), "i32", To_String (TI.Pir_Type)));
                  else
                     declare
                        Df : constant String := Next_Reg (Ctx.E, "ld");
                        Ln : constant String := Next_Reg (Ctx.E, "ll");
                        Ps : constant String := Next_Reg (Ctx.E, "lp");
                        Cl : constant String := Next_Reg (Ctx.E, "lcl");
                     begin
                        Emit_Inst (Ctx.E, Df & " = sub.i32 " & To_String (L_R) & ", " & To_String (F_R));
                        Emit_Inst (Ctx.E, Ln & " = add.i32 " & Df & ", 1");
                        Emit_Inst (Ctx.E, Ps & " = cmp_gt_s.i32 " & Ln & ", 0");
                        Emit_Inst (Ctx.E, Cl & " = select.i32 " & Ps & ", " & Ln & ", 0");
                        Res.Reg := To_Unbounded_String (Emit_Int_Convert (Ctx, Cl, "i32", To_String (TI.Pir_Type)));
                     end;
                  end if;
                  return Res;
               end;
            end;

         elsif AName = "POS" then
            declare
               V : constant Expr_Val := Emit_Expr (Ctx, Arg);
            begin
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := TI.Pir_Type;
               Res.Align    := TI.Align;
               Res.Reg      := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type)));
               return Res;
            end;

         elsif AName = "VAL" then
            declare
               V : constant Expr_Val := Emit_Expr (Ctx, Arg);
               Cv : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (TI.Pir_Type));
            begin
               Emit_Range_Check (Ctx, Cv, To_String (TI.Pir_Type), ET);
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := TI.Pir_Type;
               Res.Align    := TI.Align;
               Res.Reg      := To_Unbounded_String (Cv);
               return Res;
            end;

         elsif AName = "SUCC" or else AName = "PRED" then
            declare
               V   : constant Expr_Val := Emit_Expr (Ctx, Arg, ET);
               PT  : constant String := To_String (TI.Pir_Type);
               OutR : constant String := Next_Reg (Ctx.E, "sp");
               Op  : constant String := (if AName = "SUCC" then "add" else "sub");
            begin
               if Ctx.Enable_Checks then
                  declare
                     Ovf : constant String := Next_Reg (Ctx.E, "ov");
                  begin
                     Emit_Inst (Ctx.E, OutR & ", " & Ovf & " = " & Op & "_ovf_s." & PT & " " & To_String (V.Reg) & ", 1");
                     Emit_Check_Fail_If (Ctx, Ovf);
                  end;
                  Emit_Range_Check (Ctx, OutR, PT, ET);
               else
                  Emit_Inst (Ctx.E, OutR & " = " & Op & "." & PT & " " & To_String (V.Reg) & ", 1");
               end if;
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (OutR);
               Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "SIZE" then
            declare
               TS : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (Pfx)));
               PTI : constant Type_Info := Get_Type_Info (Ctx, TS);
               R   : constant String := Next_Reg (Ctx.E, "sz");
            begin
               Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (PTI.Byte_Size * 8));
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "ADDRESS" then
            declare
               LV : constant Expr_Val := Emit_LValue (Ctx, Pfx);
               R  : constant String := Next_Reg (Ctx.E, "ad");
            begin
               Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & To_String (LV.Reg));
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String ("i64"); Res.Ada_Type := ET; Res.Align := 8;
               return Res;
            end;

         elsif AName = "WIDTH" then
            declare
               R : constant String := Next_Reg (Ctx.E, "wd");
            begin
               Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " 11");
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "DIGITS" then
            declare
               TS  : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (Pfx)));
               Dig : constant Integer := (if Scalar_Bits (TS) <= 32 then 6 else 15);
               R   : constant String := Next_Reg (Ctx.E, "dg");
            begin
               Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Dig));
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         elsif AName = "FORE" or else AName = "AFT" then
            declare
               Val : constant Integer := (if AName = "FORE" then 2 else 9);
               R   : constant String := Next_Reg (Ctx.E, "fa");
            begin
               Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Val));
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;

         else
            declare
               R : constant String := Next_Reg (Ctx.E, "au");
            begin
               Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " 0");
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
               return Res;
            end;
         end if;
      end;
   end Emit_Attribute;

   function Emit_Expr (Ctx : in out Context; Node : TREE; Target_Type : TREE := (0, 0, 0)) return Expr_Val is
      Res : Expr_Val;
      K   : constant NODE_NAME := KIND (Node);
   begin
      case K is
         when DN_NUMERIC_LITERAL =>
            declare
               ET  : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               SV  : constant TREE := D (SM_VALUE, Node);
            begin
               if Is_Void_Tree (ET) or else KIND (ET) = DN_UNIVERSAL_INTEGER or else KIND (ET) = DN_UNIVERSAL_REAL then
                  if not Is_Void_Tree (Target_Type) and then Is_Scalar_Type (Target_Type) then
                     ET := Subst_Tree (Ctx, Target_Type);
                  end if;
               end if;
               declare
                  TI : constant Type_Info := Get_Type_Info (Ctx, ET);
                  R  : constant String := Next_Reg (Ctx.E, "c");
               begin
                  Res.Kind     := EK_Scalar;
                  Res.Ada_Type := ET;
                  Res.Pir_Type := TI.Pir_Type;
                  Res.Align    := TI.Align;
                  if Is_Float_Type (ET) then
                     Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (Eval_Float64 (SV)));
                  elsif Is_Fixed_Type (ET) then
                     declare
                        FV  : constant IEEE_Float_64 := Eval_Float64 (SV);
                        Sm  : constant IEEE_Float_64 := Fixed_Small (ET);
                        Sc  : constant Integer_64 := Integer_64 (IEEE_Float_64'Rounding (FV / Sm));
                     begin
                        Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Sc)));
                     end;
                  else
                     Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Eval_Int64 (SV))));
                  end if;
                  Res.Reg := To_Unbounded_String (R);
                  return Res;
               end;
            end;

         when DN_NULL_ACCESS =>
            declare
               ET : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               R  : constant String := Next_Reg (Ctx.E, "nil");
            begin
               Emit_Inst (Ctx.E, R & " = const.ptr 0");
               Res.Kind     := EK_Scalar;
               Res.Reg      := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String ("ptr");
               Res.Ada_Type := ET;
               Res.Align    := 8;
               return Res;
            end;

         when DN_STRING_LITERAL =>
            declare
               Raw  : constant String := Sym_Name (Node);
               Text : constant String := Decode_Ada_String_Lit (Raw);
               Len  : constant Natural := Text'Length;
               Sym  : constant String := "@__ada_str_" & Img (Next_Sym_Id (Ctx.E));
               AR   : constant String := Next_Reg (Ctx.E, "sp");
               F_R  : constant String := Next_Reg (Ctx.E, "sf");
               L_R  : constant String := Next_Reg (Ctx.E, "sl");
               ET   : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
            begin
               Add_Data_Decl (Ctx.E, Sym,
                 Sym & " = data align 1 { """ & Escape_Pir_String (Text) & "\0"" }");
               Emit_Inst (Ctx.E, AR & " = addr " & Sym);
               Emit_Inst (Ctx.E, F_R & " = const.i32 1");
               Emit_Inst (Ctx.E, L_R & " = const.i32 " & Img (Len));
               Res.Kind           := EK_Slice;
               Res.Reg            := To_Unbounded_String (AR);
               Res.Pir_Type       := To_Unbounded_String ("ptr");
               Res.Ada_Type       := ET;
               Res.Align          := 1;
               Res.NDims          := 1;
               Res.First_Regs (1) := To_Unbounded_String (F_R);
               Res.Last_Regs (1)  := To_Unbounded_String (L_R);
               return Res;
            end;

         when DN_USED_CHAR | DN_USED_OBJECT_ID =>
            declare
               Def : constant TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_DEFN, Node)));
               ET  : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
            begin
               -- Enumeration / Character literal
               if KIND (Def) = DN_ENUMERATION_ID or else KIND (Def) = DN_CHARACTER_ID then
                  declare
                     TI  : constant Type_Info := Get_Type_Info (Ctx, ET);
                     Pos : constant Integer := DI (SM_POS, Def);
                     R   : constant String := Next_Reg (Ctx.E, "en");
                  begin
                     Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Pos));
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                     return Res;
                  end;
               end if;

               -- Named number (DN_NUMBER_ID) or static constant (e.g. ASCII.LF)
               if KIND (Def) = DN_NUMBER_ID or else Has_Static_Val (D (SM_VALUE, Node)) then
                  declare
                     SV : constant TREE := D (SM_VALUE, Node);
                  begin
                     if Has_Static_Val (SV) then
                        if Is_Void_Tree (ET) or else KIND (ET) = DN_UNIVERSAL_INTEGER or else KIND (ET) = DN_UNIVERSAL_REAL then
                           if not Is_Void_Tree (Target_Type) and then Is_Scalar_Type (Target_Type) then
                              ET := Subst_Tree (Ctx, Target_Type);
                           end if;
                        end if;
                        declare
                           TI : constant Type_Info := Get_Type_Info (Ctx, ET);
                           R  : constant String := Next_Reg (Ctx.E, "nc");
                        begin
                           if Is_Float_Type (ET) then
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (Eval_Float64 (SV)));
                           elsif Is_Fixed_Type (ET) then
                              declare
                                 Sc : constant Integer_64 := Integer_64 (IEEE_Float_64'Rounding (Eval_Float64 (SV) / Fixed_Small (ET)));
                              begin
                                 Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Sc)));
                              end;
                           else
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (Eval_Int64 (SV))));
                           end if;
                           Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := ET; Res.Align := TI.Align;
                           return Res;
                        end;
                     end if;
                  end;
               end if;

               -- Variable / Parameter / Constant in Var_Maps
               declare
                  Key : constant Unsigned_32 := Tree_Key (Def);
               begin
                  if Ctx.Vars.Contains (Key) then
                     declare
                        V  : constant Var_Info := Ctx.Vars.Element (Key);
                        TI : constant Type_Info := Get_Type_Info (Ctx, V.Ada_Type);
                     begin
                        if V.Kind = VK_Const_Int then
                           declare
                              R : constant String := Next_Reg (Ctx.E, "ci");
                           begin
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Long_Long_Integer (V.Const_I64)));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                              return Res;
                           end;
                        elsif V.Kind = VK_Const_Float then
                           declare
                              R : constant String := Next_Reg (Ctx.E, "cf");
                           begin
                              Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Format_Float64 (V.Const_F64));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                              return Res;
                           end;
                        elsif V.Kind = VK_Param_Val then
                           Res.Kind := EK_Scalar; Res.Reg := V.Pir_Name;
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                           return Res;
                        elsif V.Kind = VK_Uncon_Array_Param or else V.Kind = VK_Dyn_Array then
                           Res.Kind       := EK_Slice;
                           Res.Reg        := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                           Res.Pir_Type   := To_Unbounded_String ("ptr");
                           Res.Ada_Type   := V.Ada_Type;
                           Res.Align      := V.Align;
                           Res.NDims      := V.NDims;
                           Res.First_Regs := V.First_Regs;
                           Res.Last_Regs  := V.Last_Regs;
                           return Res;
                        elsif TI.Kind = TK_Scalar then
                           declare
                              Addr : constant String := Emit_Var_Addr (Ctx, V);
                              R    : constant String := Next_Reg (Ctx.E, "ld");
                           begin
                              Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & Addr & ", align " & Img (TI.Align));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                              return Res;
                           end;
                        else
                           Res.Kind := EK_Addr;
                           Res.Reg  := To_Unbounded_String (Emit_Var_Addr (Ctx, V));
                           Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := V.Ada_Type; Res.Align := TI.Align;
                           return Res;
                        end if;
                     end;
                  end if;
               end;

               -- Fallback: external package global via Emit_LValue
               declare
                  LV : constant Expr_Val := Emit_LValue (Ctx, Node);
                  TI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
               begin
                  if TI.Kind = TK_Scalar and then LV.Kind = EK_Addr then
                     declare
                        R : constant String := Next_Reg (Ctx.E, "gld");
                     begin
                        Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (TI.Align));
                        Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                        Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := LV.Ada_Type; Res.Align := TI.Align;
                        return Res;
                     end;
                  end if;
                  return LV;
               end;
            end;

         when DN_SELECTED | DN_INDEXED | DN_ALL | DN_SLICE =>
            declare
               LV : constant Expr_Val := Emit_LValue (Ctx, Node);
               TI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
            begin
               if LV.Kind = EK_Addr and then TI.Kind = TK_Scalar then
                  declare
                     R : constant String := Next_Reg (Ctx.E, "ld");
                  begin
                     Emit_Inst (Ctx.E, R & " = load." & To_String (TI.Pir_Type) & " " & To_String (LV.Reg) & ", align " & Img (TI.Align));
                     Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                     Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := LV.Ada_Type; Res.Align := TI.Align;
                     return Res;
                  end;
               end if;
               return LV;
            end;

         when DN_FUNCTION_CALL | DN_NULLARY_CALL =>
            return Emit_Subp_Call (Ctx, Node, Is_Func => True);

         when DN_PARENTHESIZED =>
            return Emit_Expr (Ctx, D (AS_EXP, Node), Target_Type);

         when DN_QUALIFIED =>
            declare
               ET : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               E  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Node), ET);
            begin
               if E.Kind = EK_Scalar then
                  Emit_Range_Check (Ctx, To_String (E.Reg), To_String (E.Pir_Type), ET);
               end if;
               return E;
            end;

         when DN_CONVERSION =>
            declare
               ET  : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               ETI : constant Type_Info := Get_Type_Info (Ctx, ET);
               Src : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Node));
               ST  : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, Src.Ada_Type));
            begin
               if ETI.Kind /= TK_Scalar then
                  Res := Src;
                  Res.Ada_Type := ET;
                  return Res;
               end if;
               Res.Kind     := EK_Scalar;
               Res.Ada_Type := ET;
               Res.Pir_Type := ETI.Pir_Type;
               Res.Align    := ETI.Align;

               if Is_Float_Type (ET) and then Is_Float_Type (ST) then
                  if To_String (Src.Pir_Type) = To_String (ETI.Pir_Type) then
                     Res.Reg := Src.Reg;
                  else
                     declare
                        R : constant String := Next_Reg (Ctx.E, "fc");
                     begin
                        if To_String (ETI.Pir_Type) = "f64" then
                           Emit_Inst (Ctx.E, R & " = fpext.f64 " & To_String (Src.Reg));
                        else
                           Emit_Inst (Ctx.E, R & " = fptrunc.f32 " & To_String (Src.Reg));
                        end if;
                        Res.Reg := To_Unbounded_String (R);
                     end;
                  end if;
               elsif Is_Float_Type (ET) and then Is_Fixed_Type (ST) then
                  declare
                     Fl : constant String := Next_Reg (Ctx.E, "x2f");
                     Sc : constant String := Next_Reg (Ctx.E, "xsc");
                     Sm : constant IEEE_Float_64 := Fixed_Small (ST);
                  begin
                     Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (Src.Reg));
                     Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Sm));
                     if To_String (ETI.Pir_Type) = "f32" then
                        declare
                           Tr : constant String := Next_Reg (Ctx.E, "xtr");
                        begin
                           Emit_Inst (Ctx.E, Tr & " = fptrunc.f32 " & Sc);
                           Res.Reg := To_Unbounded_String (Tr);
                        end;
                     else
                        Res.Reg := To_Unbounded_String (Sc);
                     end if;
                  end;
               elsif Is_Fixed_Type (ET) and then Is_Float_Type (ST) then
                  declare
                     F64 : Unbounded_String := Src.Reg;
                     Div : constant String := Next_Reg (Ctx.E, "f2x");
                     R   : constant String := Next_Reg (Ctx.E, "fx");
                     Sm  : constant IEEE_Float_64 := Fixed_Small (ET);
                  begin
                     if To_String (Src.Pir_Type) = "f32" then
                        declare
                           Ex : constant String := Next_Reg (Ctx.E, "fex");
                        begin
                           Emit_Inst (Ctx.E, Ex & " = fpext.f64 " & To_String (Src.Reg));
                           F64 := To_Unbounded_String (Ex);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, Div & " = fdiv.f64 " & To_String (F64) & ", " & Format_Float64 (Sm));
                     Emit_Inst (Ctx.E, R & " = fptosi." & To_String (ETI.Pir_Type) & " " & Div & " [sat]");
                     Res.Reg := To_Unbounded_String (R);
                  end;
               elsif Is_Float_Type (ET) and then Is_Discrete_Type (ST) then
                  declare
                     R : constant String := Next_Reg (Ctx.E, "i2f");
                  begin
                     Emit_Inst (Ctx.E, R & " = sitofp." & To_String (ETI.Pir_Type) & " " & To_String (Src.Reg));
                     Res.Reg := To_Unbounded_String (R);
                  end;
               elsif Is_Discrete_Type (ET) and then Is_Float_Type (ST) then
                  -- Ada rounds float-to-integer away from zero (add +0.5 if >= 0 else -0.5)
                  declare
                     SFT  : constant String := To_String (Src.Pir_Type);
                     Neg  : constant String := Next_Reg (Ctx.E, "rn");
                     Half : constant String := Next_Reg (Ctx.E, "rh");
                     Adj  : constant String := Next_Reg (Ctx.E, "ra");
                     R    : constant String := Next_Reg (Ctx.E, "f2i");
                  begin
                     Emit_Inst (Ctx.E, Neg & " = fcmp_olt." & SFT & " " & To_String (Src.Reg) & ", 0.0");
                     Emit_Inst (Ctx.E, Half & " = select." & SFT & " " & Neg & ", -0.5, 0.5");
                     Emit_Inst (Ctx.E, Adj & " = fadd." & SFT & " " & To_String (Src.Reg) & ", " & Half);
                     Emit_Inst (Ctx.E, R & " = fptosi." & To_String (ETI.Pir_Type) & " " & Adj & " [sat]");
                     Emit_Range_Check (Ctx, R, To_String (ETI.Pir_Type), ET);
                     Res.Reg := To_Unbounded_String (R);
                  end;
               else
                  -- Integer / discrete conversion
                  declare
                     Cv : constant String :=
                       Emit_Int_Convert (Ctx, To_String (Src.Reg), To_String (Src.Pir_Type), To_String (ETI.Pir_Type));
                  begin
                     Emit_Range_Check (Ctx, Cv, To_String (ETI.Pir_Type), ET);
                     Res.Reg := To_Unbounded_String (Cv);
                  end;
               end if;
               return Res;
            end;

         when DN_SHORT_CIRCUIT =>
            declare
               Is_And : constant Boolean := KIND (D (AS_SHORT_CIRCUIT_OP, Node)) = DN_AND_THEN;
               L      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Node));
               L_Rhs  : constant String := Next_Label (Ctx.E, "sc_rhs");
               L_End  : constant String := Next_Label (Ctx.E, "sc_end");
               OutR   : constant String := Next_Reg (Ctx.E, "sc");
            begin
               if Is_And then
                  Emit_Term (Ctx.E, "br " & To_String (L.Reg) & ", " & L_Rhs & ", " & L_End & "(0)");
               else
                  Emit_Term (Ctx.E, "br " & To_String (L.Reg) & ", " & L_End & "(1), " & L_Rhs);
               end if;
               Emit_Label (Ctx.E, L_Rhs);
               declare
                  R_V : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Node));
               begin
                  Emit_Term (Ctx.E, "jmp " & L_End & "(" & To_String (R_V.Reg) & ")");
               end;
               Emit_Label (Ctx.E, L_End, OutR & ": i8");
               Res.Kind     := EK_Scalar;
               Res.Reg      := To_Unbounded_String (OutR);
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               Res.Align    := 1;
               return Res;
            end;

         when DN_RANGE_MEMBERSHIP =>
            declare
               Is_Not : constant Boolean := KIND (D (AS_MEMBERSHIP_OP, Node)) = DN_NOT_IN;
               V      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Node));
               RT, RL, RH : Unbounded_String;
               VC     : Unbounded_String;
               C1     : constant String := Next_Reg (Ctx.E, "m1");
               C2     : constant String := Next_Reg (Ctx.E, "m2");
               InR    : constant String := Next_Reg (Ctx.E, "min");
            begin
               Emit_Discrete_Range (Ctx, D (AS_RANGE, Node), RT, RL, RH);
               VC := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), To_String (RT)));
               Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & To_String (RT) & " " & To_String (VC) & ", " & To_String (RL));
               Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & To_String (RT) & " " & To_String (VC) & ", " & To_String (RH));
               Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
               if Is_Not then
                  declare
                     N : constant String := Next_Reg (Ctx.E, "mnot");
                  begin
                     Emit_Inst (Ctx.E, N & " = xor.i8 " & InR & ", 1");
                     Res.Reg := To_Unbounded_String (N);
                  end;
               else
                  Res.Reg := To_Unbounded_String (InR);
               end if;
               Res.Kind     := EK_Scalar;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               Res.Align    := 1;
               return Res;
            end;

         when DN_TYPE_MEMBERSHIP =>
            declare
               Is_Not : constant Boolean := KIND (D (AS_MEMBERSHIP_OP, Node)) = DN_NOT_IN;
               V      : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP, Node));
               TS     : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, Get_Name_Defn (D (AS_NAME, Node))));
               St     : Boolean;
               Lo, Hi : Integer_64;
               InR    : constant String := Next_Reg (Ctx.E, "tmin");
            begin
               Get_Discrete_Bounds (TS, St, Lo, Hi);
               if St then
                  declare
                     PT : constant String := To_String (V.Pir_Type);
                     C1 : constant String := Next_Reg (Ctx.E, "tm1");
                     C2 : constant String := Next_Reg (Ctx.E, "tm2");
                  begin
                     Emit_Inst (Ctx.E, C1 & " = cmp_ge_s." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Lo)));
                     Emit_Inst (Ctx.E, C2 & " = cmp_le_s." & PT & " " & To_String (V.Reg) & ", " & Img (Long_Long_Integer (Hi)));
                     Emit_Inst (Ctx.E, InR & " = and.i8 " & C1 & ", " & C2);
                  end;
               else
                  Emit_Inst (Ctx.E, InR & " = const.i8 1");
               end if;
               if Is_Not then
                  declare
                     N : constant String := Next_Reg (Ctx.E, "tmnot");
                  begin
                     Emit_Inst (Ctx.E, N & " = xor.i8 " & InR & ", 1");
                     Res.Reg := To_Unbounded_String (N);
                  end;
               else
                  Res.Reg := To_Unbounded_String (InR);
               end if;
               Res.Kind     := EK_Scalar;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
               Res.Align    := 1;
               return Res;
            end;

         when DN_ATTRIBUTE =>
            return Emit_Attribute (Ctx, Node, Target_Type);

         when DN_AGGREGATE =>
            declare
               ET  : TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Node));
            begin
               if Is_Void_Tree (ET) or else Is_Unconstrained_Array (ET) then
                  if not Is_Void_Tree (Target_Type) then
                     ET := Subst_Tree (Ctx, Target_Type);
                  end if;
               end if;
               declare
                  TI  : constant Type_Info := Get_Type_Info (Ctx, ET);
                  Tmp : constant String := Next_Reg (Ctx.E, "aggtmp");
               begin
                  Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (TI.Pir_Type) & ", align " & Img (TI.Align));
                  Emit_Init_Composite (Ctx, Tmp, ET, Node);
                  Res.Kind     := EK_Addr;
                  Res.Reg      := To_Unbounded_String (Tmp);
                  Res.Pir_Type := TI.Pir_Type;
                  Res.Ada_Type := ET;
                  Res.Align    := TI.Align;
                  return Res;
               end;
            end;

         when DN_QUALIFIED_ALLOCATOR | DN_SUBTYPE_ALLOCATOR =>
            declare
               ET    : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Node)));
               Des_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, ET)));
               DTI   : constant Type_Info := Get_Type_Info (Ctx, Des_T);
               Ptr_R : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_alloc", Img (Natural'Max (1, DTI.Byte_Size)), "ptr", Ptr_R);
               if K = DN_QUALIFIED_ALLOCATOR then
                  declare
                     Q : constant TREE := D (AS_QUALIFIED, Node);
                     Init_E : constant TREE := D (AS_EXP, Q);
                  begin
                     if DTI.Kind = TK_Scalar then
                        declare
                           V : constant Expr_Val := Emit_Expr (Ctx, Init_E, Des_T);
                        begin
                           Emit_Inst (Ctx.E,
                             "store." & To_String (DTI.Pir_Type) & " " & To_String (V.Reg) &
                             ", " & To_String (Ptr_R) & ", align " & Img (DTI.Align));
                        end;
                     else
                        Emit_Init_Composite (Ctx, To_String (Ptr_R), Des_T, Init_E);
                     end if;
                  end;
               end if;
               Res.Kind     := EK_Scalar;
               Res.Reg      := Ptr_R;
               Res.Pir_Type := To_Unbounded_String ("ptr");
               Res.Ada_Type := ET;
               Res.Align    := 8;
               return Res;
            end;

         when others =>
            declare
               R : constant String := Next_Reg (Ctx.E, "unk");
            begin
               Emit_Comment (Ctx.E, "Unhandled expr node: " & NODE_IMAGE (K));
               Emit_Inst (Ctx.E, R & " = const.i32 0");
               Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
               Res.Pir_Type := To_Unbounded_String ("i32"); Res.Align := 4;
               return Res;
            end;
      end case;
   end Emit_Expr;

end Cg_Expr;
