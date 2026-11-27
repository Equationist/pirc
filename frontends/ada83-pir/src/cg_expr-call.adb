-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with Ada.Containers.Vectors;
with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;
with Cg_Expr.Ops; use Cg_Expr.Ops;
with Cg_Expr.RTL; use Cg_Expr.RTL;

package body Cg_Expr.Call is

   -- Compute the static link pointer to pass when calling Callee_SI from Ctx.Cur_Subp_Key
   function Compute_Call_SL (Ctx : in out Context; Callee_SI : Subp_Info) return String is
   begin
      if Callee_SI.Parent_Subp = Ctx.Cur_Subp_Key then
         if Length (Ctx.Cur_Frame_Reg) > 0 then
            return To_String (Ctx.Cur_Frame_Reg);
         else
            return "0";
         end if;
      end if;
      declare
         Cur_K : Unsigned_32 := Ctx.Cur_Subp_Key;
         SL    : Unbounded_String := Ctx.Cur_SL_Reg;
      begin
         if Length (SL) = 0 then
            return "0";
         end if;
         for Hop in 1 .. 16 loop
            exit when not Ctx.Subps.Contains (Cur_K);
            declare
               SI : constant Subp_Info := Ctx.Subps.Element (Cur_K);
               Parent_K : constant Unsigned_32 := SI.Parent_Subp;
            begin
               if Parent_K = Callee_SI.Parent_Subp then
                  return (if Length (SL) > 0 then To_String (SL) else "0");
               elsif Parent_K = 0 or else not Ctx.Subps.Contains (Parent_K) then
                  return "0";
               else
                  declare
                     PI  : constant Subp_Info := Ctx.Subps.Element (Parent_K);
                     FP  : constant String := Next_Reg (Ctx.E, "cslp");
                     NSL : constant String := Next_Reg (Ctx.E, "csl");
                  begin
                     if Length (PI.Frame_Type) = 0 then
                        return "0";
                     end if;
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
      Param_L           : SEQ_TYPE := MAKE;
      Res_Type          : TREE := CONST_VOID;
      Orig_Derived_Spec : TREE := CONST_VOID;
      Orig_Derived_Ret  : TREE := CONST_VOID;
      Invert_Bool       : Boolean := False;
      Res               : Expr_Val;
   begin
      while not Is_Void_Tree (Des_Node) and then KIND (Des_Node) = DN_SELECTED loop
         Des_Node := D (AS_DESIGNATOR, Des_Node);
      end loop;
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
      -- Resolve prefix of an attribute (supports T'BASE, Pkg.T, objects, components, indexed)
      if KIND (Des_Node) = DN_ATTRIBUTE then
         -- Attribute function call: POS, VAL, SUCC, PRED, IMAGE, VALUE
         declare
            Attr_Id : constant TREE := D (AS_USED_NAME_ID, Des_Node);
            AName   : constant String := Sym_Name (Attr_Id);
            Pfx_N   : constant TREE := D (AS_NAME, Des_Node);
            ET      : constant TREE := Res_Type;
            TI      : constant Type_Info := Get_Type_Info (Ctx, ET);
            Arg_N   : TREE := CONST_VOID;
            PL      : SEQ_TYPE := Param_L;

            function Attr_Pfx_Type (P : TREE) return TREE is
            begin
               if Is_Void_Tree (P) then
                  return CONST_VOID;
               elsif KIND (P) = DN_ATTRIBUTE then
                  if Sym_Name (D (AS_USED_NAME_ID, P)) = "BASE" then
                     return Subst_Base_Type (Ctx, Attr_Pfx_Type (D (AS_NAME, P)));
                  end if;
                  return Subst_Tree (Ctx, D (SM_EXP_TYPE, P));
               elsif KIND (P) = DN_SELECTED then
                  declare
                     Des : constant TREE := D (AS_DESIGNATOR, P);
                     Df  : constant TREE := D (SM_DEFN, Des);
                  begin
                     if not Is_Void_Tree (Df) and then KIND (Df) in CLASS_TYPE_NAME then
                        return Subst_Tree (Ctx, D (SM_TYPE_SPEC, Df));
                     end if;
                     return Subst_Tree (Ctx, D (SM_EXP_TYPE, P));
                  end;
               elsif KIND (P) = DN_USED_NAME_ID and then not Is_Void_Tree (D (SM_DEFN, P))
                 and then KIND (D (SM_DEFN, P)) in CLASS_TYPE_NAME
               then
                  return Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (SM_DEFN, P)));
               elsif KIND (P) in CLASS_EXP and then KIND (P) /= DN_USED_NAME_ID and then KIND (P) /= DN_USED_OP then
                  return Subst_Tree (Ctx, D (SM_EXP_TYPE, P));
               else
                  return CONST_VOID;
               end if;
            end Attr_Pfx_Type;
         begin
            if not IS_EMPTY (PL) then
               POP (PL, Arg_N);
            end if;
            return Emit_Discrete_Attr_Op (Ctx, AName, Attr_Pfx_Type (Pfx_N), ET, Arg_N);
         end;
      end if;
      Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des_Node)));

      -- 0. Unwrap derived subprograms (DN_DERIVED_SUBPROG) and renamed subprograms (DN_RENAMES_UNIT)
      for Unwrap_Step in 1 .. 16 loop
         exit when Is_Void_Tree (Def) or else KIND (Def) not in CLASS_SUBPROG_NAME;
         declare
            UD : TREE := D (SM_UNIT_DESC, Def);
         begin
            if Is_Void_Tree (UD) and then Ctx.Subp_Renames.Contains (Tree_Key (Def)) then
               UD := Ctx.Subp_Renames.Element (Tree_Key (Def));
            end if;
            exit when Is_Void_Tree (UD);
            if KIND (UD) = DN_DERIVED_SUBPROG then
               -- LRM 3.4(6) / AI83-00237 (c34018a, c34005*): derived subprogram calls check the PARENT
               -- subprogram's formal/return constraints (converting to/from Derived'Base), NOT the derived subtype!
               Def := Canonical_Def (Subst_Tree (Ctx, D (SM_DERIVABLE, UD)));
            elsif KIND (UD) = DN_RENAMES_UNIT then
               declare
                  RN : constant TREE := D (AS_NAME, UD);
               begin
                  exit when Is_Void_Tree (RN);
                  if KIND (RN) = DN_ATTRIBUTE then
                     -- Renamed attribute function (e.g. function Next renames T'Succ, T'Image, T'Value)
                     declare
                        Attr_Id  : constant TREE := D (AS_USED_NAME_ID, RN);
                        AName    : constant String := Sym_Name (Attr_Id);
                        Pfx_N    : constant TREE := D (AS_NAME, RN);
                        Pfx_Base : constant TREE :=
                          (if not Is_Void_Tree (Pfx_N) and then KIND (Pfx_N) = DN_ATTRIBUTE
                           then D (AS_NAME, Pfx_N) else Pfx_N);
                        Pfx_Def  : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Pfx_Base)));
                        Pfx_TS   : constant TREE :=
                          (if Is_Void_Tree (Pfx_Def) then CONST_VOID
                           elsif KIND (Pfx_Def) in CLASS_TYPE_NAME
                           then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_TYPE_SPEC, Pfx_Def)))
                           else Resolve_Type_Spec (Pfx_Def));
                        Arg_N    : TREE := CONST_VOID;
                        PL       : SEQ_TYPE := Param_L;
                     begin
                        if not IS_EMPTY (PL) then POP (PL, Arg_N); end if;
                        if AName = "POS" or else AName = "VAL" or else AName = "SUCC"
                          or else AName = "PRED" or else AName = "IMAGE" or else AName = "VALUE"
                        then
                           return Emit_Discrete_Attr_Op (Ctx, AName, Pfx_TS, Res_Type, Arg_N);
                        end if;
                        exit;
                     end;
                  else
                     Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (RN)));
                  end if;
               end;
            else
               exit;
            end if;
         end;
      end loop;

      -- 0b. Renamed enumeration/character literal called as parameterless function!
      if not Is_Void_Tree (Def) and then (KIND (Def) = DN_ENUMERATION_ID or else KIND (Def) = DN_CHARACTER_ID) then
         declare
            TI  : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
            Pos : constant Integer := DI (SM_POS, Def);
            R   : constant String := Next_Reg (Ctx.E, "ren");
         begin
            Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " " & Img (Pos));
            Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
            Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := TI.Align;
            return Res;
         end;
      end if;

      -- 1. Built-in operator
      if not Is_Void_Tree (Def) and then KIND (Def) = DN_BLTN_OPERATOR_ID then
         -- LRM 4.10 (c4a010a): fold exact static universal_real expressions/comparisons from semphase SM_VALUE
         declare
            SV : constant TREE := Safe_SM_Value (Call_Node);
         begin
            if Has_Static_Val (SV) and then not IS_EMPTY (Param_L) then
               declare
                  A1   : constant TREE := HEAD (Param_L);
                  A1_T : constant TREE :=
                    (if not Is_Void_Tree (A1) and then KIND (A1) in CLASS_EXP
                     then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, A1)))
                     else CONST_VOID);
                  A2_T : TREE := CONST_VOID;
               begin
                  if not IS_EMPTY (TAIL (Param_L)) then
                     declare
                        A2 : constant TREE := HEAD (TAIL (Param_L));
                     begin
                        if not Is_Void_Tree (A2) and then KIND (A2) in CLASS_EXP then
                           A2_T := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, A2)));
                        end if;
                     end;
                  end if;
                  if (not Is_Void_Tree (A1_T) and then KIND (A1_T) in DN_UNIVERSAL_REAL | DN_UNIVERSAL_INTEGER)
                    or else (not Is_Void_Tree (A2_T) and then KIND (A2_T) = DN_UNIVERSAL_REAL)
                    or else Is_Discrete_Type (Res_Type)
                    or else Is_Fixed_Type (Res_Type)
                  then
                     declare
                        RTI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
                        R   : constant String := Next_Reg (Ctx.E, "urf");
                     begin
                        if Is_Float_Type (Res_Type) then
                           Res.Kind := EK_Scalar;
                           Res.Reg := To_Unbounded_String
                             (Emit_Float_Const_Safe (Ctx, Eval_Float64 (SV), To_String (RTI.Pir_Type), "urf"));
                           Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                           return Res;
                        elsif Is_Fixed_Type (Res_Type) then
                           declare
                              Tick_V : Integer_64;
                              Tick_O : Boolean;
                           begin
                              Eval_Fixed_Ticks_Exact_Checked (SV, Res_Type, Tick_V, Tick_O);
                              if Tick_O and then Ctx.Enable_Checks then
                                 Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                                 declare
                                    Unr : constant String := Next_Label (Ctx.E, "sfx_u");
                                 begin
                                    Emit_Label (Ctx.E, Unr);
                                 end;
                              end if;
                              Emit_Inst (Ctx.E, R & " = const." & To_String (RTI.Pir_Type) & " " & Img (Long_Long_Integer (Tick_V)));
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                              return Res;
                           end;
                        elsif Is_Discrete_Type (Res_Type) then
                           declare
                              V64 : Integer_64;
                              Ovf : Boolean;
                              BT  : constant TREE := Subst_Base_Type (Ctx, Res_Type);
                              StB : Boolean := False;
                              BLo, BHi : Integer_64 := 0;
                           begin
                              Eval_Int64_Checked (SV, V64, Ovf);
                              if not Is_Void_Tree (BT) and then Is_Discrete_Type (BT) then
                                 Get_Discrete_Bounds (BT, StB, BLo, BHi);
                              end if;
                              if Ctx.Enable_Checks and then (Ovf or else (StB and then (V64 < BLo or else V64 > BHi))) then
                                 Emit_Raise_Tag (Ctx, Constraint_Error_Tag (Ctx));
                                 declare
                                    Unr : constant String := Next_Label (Ctx.E, "sdo_u");
                                 begin
                                    Emit_Label (Ctx.E, Unr);
                                 end;
                                 V64 := 0;
                              end if;
                              if not Ovf then
                                 Emit_Inst (Ctx.E, R & " = const." & To_String (RTI.Pir_Type) & " " & Format_Int_Imm_For_Pir (V64, To_String (RTI.Pir_Type)));
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                                 return Res;
                              end if;
                           end;
                        end if;
                     end;
                  end if;
               end;
            end if;
         end;
         return Emit_Builtin_Op (Ctx, Def, Param_L, Res_Type);
      end if;

      -- Guard non-subprogram targets (e.g. task entries, unhandled renames)
      if Is_Void_Tree (Def) or else KIND (Def) not in CLASS_SUBPROG_NAME then
         if Is_Func and then not Is_Void_Tree (Res_Type) then
            declare
               TI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
               R  : constant String := Next_Reg (Ctx.E, "ns");
            begin
               if TI.Kind = TK_Scalar then
                  Emit_Inst (Ctx.E, R & " = const." & To_String (TI.Pir_Type) & " 0");
                  Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                  Res.Pir_Type := TI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := TI.Align;
               end if;
            end;
         end if;
         return Res;
      end if;

      -- 2. Implicit /= on user-defined "=" (unwrap renamings/derived/builtin "=" properly!)
      if KIND (Def) in CLASS_SUBPROG_NAME then
         declare
            UD : constant TREE := D (SM_UNIT_DESC, Def);
         begin
            if not Is_Void_Tree (UD) and then KIND (UD) = DN_IMPLICIT_NOT_EQ then
               declare
                  Eq_Def  : TREE := Canonical_Def (Subst_Tree (Ctx, D (SM_EQUAL, UD)));
                  Eq_Call : Expr_Val;
                  Inv     : constant String := Next_Reg (Ctx.E, "ineq");
               begin
                  -- Unwrap any renaming / derived chain on Eq_Def
                  for UStep in 1 .. 16 loop
                     exit when Is_Void_Tree (Eq_Def) or else KIND (Eq_Def) not in CLASS_SUBPROG_NAME;
                     declare
                        EUD : TREE := D (SM_UNIT_DESC, Eq_Def);
                     begin
                        if Is_Void_Tree (EUD) and then Ctx.Subp_Renames.Contains (Tree_Key (Eq_Def)) then
                           EUD := Ctx.Subp_Renames.Element (Tree_Key (Eq_Def));
                        end if;
                        exit when Is_Void_Tree (EUD);
                        if KIND (EUD) = DN_DERIVED_SUBPROG then
                           Eq_Def := Canonical_Def (Subst_Tree (Ctx, D (SM_DERIVABLE, EUD)));
                        elsif KIND (EUD) = DN_RENAMES_UNIT then
                           declare
                              ERN : constant TREE := D (AS_NAME, EUD);
                           begin
                              exit when Is_Void_Tree (ERN);
                              Eq_Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (ERN)));
                           end;
                        else
                           exit;
                        end if;
                     end;
                  end loop;
                  if not Is_Void_Tree (Eq_Def) and then KIND (Eq_Def) = DN_BLTN_OPERATOR_ID then
                     Eq_Call := Emit_Builtin_Op (Ctx, Eq_Def, Param_L, Res_Type);
                     Emit_Inst (Ctx.E, Inv & " = xor.i8 " & To_String (Eq_Call.Reg) & ", 1");
                     Eq_Call.Reg := To_Unbounded_String (Inv);
                     return Eq_Call;
                  elsif not Is_Void_Tree (Eq_Def) and then KIND (Eq_Def) in CLASS_SUBPROG_NAME then
                     -- P3-3.9: Reuse section 4 with Def := Eq_Def and Invert_Bool := True (preserves static link, composite & derived checks!)
                     Def := Eq_Def;
                     Invert_Bool := True;
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
            if not Is_Void_Tree (D (SM_INTERFACE, Def)) then
               SI.Is_C_Interface := Is_C_Interfaced (Def);
               if SI.Is_C_Interface then
                  SI.Needs_SL := False;
               end if;
               Ensure_Extern_Subp (Ctx, Def, SI);
            end if;
         else
            -- External subprogram from a with'd package spec or ancestor subunit
            declare
               Spec : constant TREE := D (SM_SPEC, Def);
            begin
               SI.Spec_Node      := Spec;
               SI.Pir_Name       := To_Unbounded_String (Mangle_Subp_Name (Ctx, Def, Spec, Is_Subunit => Ctx.In_Subunit));
               SI.Is_Function    := KIND (Spec) = DN_FUNCTION_SPEC;
               SI.Is_External    := True;
               SI.Is_C_Interface := Is_C_Interfaced (Def);
               if SI.Is_C_Interface then
                  SI.Needs_SL := False;
               end if;
               if SI.Is_Function then
                  declare
                     RDef : constant TREE := Get_Name_Defn (D (AS_NAME, Spec));
                  begin
                     SI.Ret_Type := (if not Is_Void_Tree (RDef) then Subst_Tree (Ctx, D (SM_TYPE_SPEC, RDef)) else CONST_VOID);
                  end;
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
            if not SI.Is_C_Interface then
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
            end if;

            -- Walk formal parameters in lockstep with actuals
            declare
               type Post_Acc_Check is record
                  Act_Ptr   : Unbounded_String;
                  Act_Type  : TREE;
                  Is_Record : Boolean := False;
               end record;
               package PAC_Vecs is new Ada.Containers.Vectors (Natural, Post_Acc_Check);
               Post_Checks : PAC_Vecs.Vector;
               type Conv_Chain_Arr is array (1 .. 8) of TREE;
               type Post_Conv_Out is record
                  Tmp_Ptr    : Unbounded_String;
                  Dst_Ptr    : Unbounded_String;
                  Src_Pir    : Unbounded_String;
                  Dst_Pir    : Unbounded_String;
                  Src_Align  : Natural := 4;
                  Dst_Align  : Natural := 4;
                  Conv_Chain : Conv_Chain_Arr := (others => CONST_VOID);
                  N_Convs    : Natural := 0;
                  Deriv_FT   : TREE := CONST_VOID;
                  Src_Type   : TREE := CONST_VOID;
                  Dst_Type   : TREE := CONST_VOID;
                  Is_BF      : Boolean := False;
                  BF_FE      : Field_Entry;
                  Is_Shared  : Boolean := False;
               end record;
               package PCO_Vecs is new Ada.Containers.Vectors (Natural, Post_Conv_Out);
               Conv_Outs : PCO_Vecs.Vector;
               type Post_Rec_Conv is record
                  Tmp_Ptr        : Unbounded_String;
                  Dst_Ptr        : Unbounded_String;
                  Src_T          : TREE := CONST_VOID;
                  Dst_T          : TREE := CONST_VOID;
                  Check_Dst_Disc : Boolean := False;
               end record;
               package PRC_Vecs is new Ada.Containers.Vectors (Natural, Post_Rec_Conv);
               Rec_Convs : PRC_Vecs.Vector;

               function Is_Named_No_Others_Agg (AN : TREE; Rem_D : Natural) return Boolean is
               begin
                  if Rem_D = 0 then return True; end if;
                  if Is_Void_Tree (AN) or else KIND (AN) /= DN_AGGREGATE then return False; end if;
                  declare
                     AL : SEQ_TYPE := LIST (D (AS_GENERAL_ASSOC_S, AN));
                     IT : TREE;
                     Has_Any : Boolean := False;
                  begin
                     while not IS_EMPTY (AL) loop
                        POP (AL, IT);
                        Has_Any := True;
                        if KIND (IT) not in DN_ASSOC | DN_NAMED then
                           return False;
                        end if;
                        declare
                           CS : SEQ_TYPE := LIST (D (AS_CHOICE_S, IT));
                           Ch : TREE;
                        begin
                           while not IS_EMPTY (CS) loop
                              POP (CS, Ch);
                              if KIND (Ch) = DN_CHOICE_OTHERS then return False; end if;
                           end loop;
                        end;
                        if Rem_D > 1 and then not Is_Named_No_Others_Agg (D (AS_EXP, IT), Rem_D - 1) then
                           return False;
                        end if;
                     end loop;
                     return Has_Any;
                  end;
               end Is_Named_No_Others_Agg;

               function Try_Get_Bitfield_Target
                 (Expr_N   : TREE;
                  Rec_Ptr  : out Unbounded_String;
                  FE_Out   : out Field_Entry;
                  Comp_Typ : out TREE) return Boolean
               is
               begin
                  Rec_Ptr  := Null_Unbounded_String;
                  Comp_Typ := CONST_VOID;
                  if Is_Void_Tree (Expr_N) then
                     return False;
                  end if;
                  -- P3-3.16: Recognize object renaming of a bitfield record component!
                  if KIND (Expr_N) = DN_USED_OBJECT_ID
                    or else (KIND (Expr_N) = DN_SELECTED
                             and then not Is_Void_Tree (Get_Name_Defn (D (AS_DESIGNATOR, Expr_N)))
                             and then KIND (Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (D (AS_DESIGNATOR, Expr_N)))))
                                      not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID)
                  then
                     declare
                        OD : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Expr_N)));
                     begin
                        if not Is_Void_Tree (OD) and then Ctx.Vars.Contains (Tree_Key (OD)) then
                           declare
                              VO : constant Var_Info := Ctx.Vars.Element (Tree_Key (OD));
                           begin
                              if VO.Is_Bitfield then
                                 Rec_Ptr  := To_Unbounded_String (Emit_Var_Addr (Ctx, VO));
                                 FE_Out   := VO.BF_FE;
                                 Comp_Typ := VO.Ada_Type;
                                 return True;
                              end if;
                           end;
                        end if;
                     end;
                     return False;
                  end if;
                  if KIND (Expr_N) /= DN_SELECTED then
                     return False;
                  end if;
                  declare
                     Des : TREE := D (AS_DESIGNATOR, Expr_N);
                     Def : TREE;
                  begin
                     while not Is_Void_Tree (Des) and then KIND (Des) = DN_SELECTED loop
                        Des := D (AS_DESIGNATOR, Des);
                     end loop;
                     Def := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des)));
                     if Is_Void_Tree (Def) or else KIND (Def) not in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                        return False;
                     end if;
                     declare
                        Pfx_E  : constant TREE := D (AS_NAME, Expr_N);
                        Pfx_Ts : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                        Rec_Ts : constant TREE :=
                          (if Is_Access_Type (Pfx_Ts) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_Ts)))
                           else Pfx_Ts);
                        Dummy_RTI : constant Type_Info := Get_Type_Info (Ctx, Rec_Ts);
                        pragma Unreferenced (Dummy_RTI);
                        CK : constant Unsigned_32 := Lookup_Record_Field_Key (Ctx, Rec_Ts, Def);
                     begin
                        if not Ctx.Fields.Contains (CK) then
                           return False;
                        end if;
                        FE_Out   := Ctx.Fields.Element (CK);
                        Comp_Typ := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def));
                        if not Uses_Bitfield_Access (Ctx, FE_Out, Comp_Typ) then
                           return False;
                        end if;
                        if Get_Type_Info (Ctx, Comp_Typ).Kind /= TK_Scalar then
                           return False;
                        end if;
                        declare
                           PLV : constant Expr_Val := Emit_LValue (Ctx, Pfx_E);
                           PT_P : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, PLV.Ada_Type));
                        begin
                           Rec_Ptr := PLV.Reg;
                           if Is_Access_Type (PT_P) then
                              if PLV.Kind = EK_Addr then
                                 declare
                                    LP : constant String := Next_Reg (Ctx.E, "bfap");
                                 begin
                                    Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (Rec_Ptr) & ", align 8");
                                    Rec_Ptr := To_Unbounded_String (LP);
                                 end;
                              end if;
                              Emit_Null_Check (Ctx, To_String (Rec_Ptr));
                           end if;
                           -- P3-6.3: Run variant discriminant check before bitfield OUT/IN OUT access (LRM 4.1.3(16))
                           if FE_Out.In_Union then
                              Emit_Variant_Check_For_Field (Ctx, To_String (Rec_Ptr), Rec_Ts, Def, CK);
                           end if;
                        end;
                        return True;
                     end;
                  end;
               end Try_Get_Bitfield_Target;
            begin
            if not Is_Void_Tree (SI.Spec_Node) then
               declare
                  PS : constant TREE := D (AS_PARAM_S, SI.Spec_Node);
                  FL : SEQ_TYPE := (if Is_Void_Tree (PS) then MAKE else LIST (PS));
                  AL : SEQ_TYPE := Param_L;
                  FP : TREE;
                  -- P3-3.10: Collect derived formal types in positional order when calling a derived subprogram
                  Deriv_FTs : array (1 .. 64) of TREE := (others => CONST_VOID);
                  N_Deriv_F : Natural := 0;
                  Param_Pos : Natural := 0;
               begin
                  if not Is_Void_Tree (Orig_Derived_Spec) and then not Is_Void_Tree (D (AS_PARAM_S, Orig_Derived_Spec)) then
                     declare
                        DFL : SEQ_TYPE := LIST (D (AS_PARAM_S, Orig_Derived_Spec));
                        DFP : TREE;
                     begin
                        while not IS_EMPTY (DFL) loop
                           POP (DFL, DFP);
                           declare
                              DFNS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, DFP));
                              DFID : TREE;
                           begin
                              while not IS_EMPTY (DFNS) and then N_Deriv_F < 64 loop
                                 POP (DFNS, DFID);
                                 N_Deriv_F := N_Deriv_F + 1;
                                 Deriv_FTs (N_Deriv_F) := Subst_Tree (Ctx, D (SM_OBJ_TYPE, DFID));
                              end loop;
                           end;
                        end loop;
                     end;
                  end if;
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
                           Param_Pos := Param_Pos + 1;
                           if not IS_EMPTY (AL) then
                              POP (AL, Act);
                           else
                              Act := D (SM_INIT_EXP, FID);
                           end if;
                           declare
                              FT  : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, FID));
                              DFT : constant TREE := (if Param_Pos <= N_Deriv_F then Deriv_FTs (Param_Pos) else CONST_VOID);
                              FTI : constant Type_Info := Get_Type_Info (Ctx, FT);
                           begin
                              if FTI.Kind = TK_Unconstrained_Array
                                or else (FTI.Kind = TK_Constrained_Array and then FTI.Is_Dynamic)
                              then
                                 declare
                                    AV : Expr_Val;
                                 begin
                                    if Mode = DN_IN then
                                       AV := Emit_Expr (Ctx, Act, FT);
                                    else
                                       AV := Emit_LValue (Ctx, Act);
                                    end if;
                                    if FTI.Kind = TK_Constrained_Array then
                                       -- LRM 6.4.1(5) (cc1311b, P3-3.7): constrained array formal requires exact bounds match
                                       -- (positional/others aggregate & string literal slide; named-without-others checks exact bounds!)
                                       declare
                                          Tgt : Expr_Val;
                                          ND_A : constant Natural := Natural'Max (1, Natural'Min (FTI.NDims, Max_Array_Dims));
                                          Act_U : TREE := Act;
                                       begin
                                          while not Is_Void_Tree (Act_U) and then KIND (Act_U) = DN_PARENTHESIZED loop
                                             Act_U := D (AS_EXP, Act_U);
                                          end loop;
                                          Tgt.Kind := EK_Slice; Tgt.Ada_Type := FT; Tgt.NDims := ND_A;
                                          for D_Idx in 1 .. ND_A loop
                                             Emit_Index_Bounds (Ctx, FTI.Dims (D_Idx).Idx_Type, Tgt.First_Regs (D_Idx), Tgt.Last_Regs (D_Idx));
                                          end loop;
                                          if not Is_Void_Tree (Act_U)
                                            and then (KIND (Act_U) = DN_STRING_LITERAL
                                                      or else (KIND (Act_U) = DN_AGGREGATE and then not Is_Named_No_Others_Agg (Act_U, ND_A)))
                                          then
                                             Emit_Array_Length_Check (Ctx, Tgt, AV, ND_A);
                                          elsif Ctx.Enable_Checks then
                                             declare
                                                AV_W : constant Array_View := Resolve_Array_View (Ctx, AV);
                                             begin
                                                Emit_Exact_Bounds_Check (Ctx, AV_W.Firsts, AV_W.Lasts, Tgt.First_Regs, Tgt.Last_Regs, ND_A);
                                             end;
                                          end if;
                                          Add_Arg (To_String (AV.Reg));
                                          if not SI.Is_C_Interface then
                                             for D_Idx in 1 .. ND_A loop
                                                Add_Arg (To_String (Tgt.First_Regs (D_Idx)));
                                                Add_Arg (To_String (Tgt.Last_Regs (D_Idx)));
                                             end loop;
                                          end if;
                                       end;
                                    else
                                       if not Is_Void_Tree (DFT) and then Ctx.Enable_Checks then
                                          declare
                                             DFTI : constant Type_Info := Get_Type_Info (Ctx, DFT);
                                          begin
                                             if DFTI.Kind = TK_Constrained_Array then
                                                declare
                                                   AV_W   : constant Array_View := Resolve_Array_View (Ctx, AV);
                                                   DF, DL : Str_Array_4;
                                                   ND_D   : constant Natural := Natural'Min (DFTI.NDims, Max_Array_Dims);
                                                begin
                                                   for D_Idx in 1 .. ND_D loop
                                                      Emit_Index_Bounds (Ctx, DFTI.Dims (D_Idx).Idx_Type, DF (D_Idx), DL (D_Idx));
                                                   end loop;
                                                   Emit_Exact_Bounds_Check (Ctx, AV_W.Firsts, AV_W.Lasts, DF, DL, ND_D);
                                                end;
                                             end if;
                                          end;
                                       end if;
                                       Add_Arg (To_String (AV.Reg));
                                       if not SI.Is_C_Interface then
                                          for D_Idx in 1 .. FTI.NDims loop
                                             declare
                                                F_R, L_R : Unbounded_String;
                                             begin
                                                Get_Array_Dim_Bounds (Ctx, AV, D_Idx, F_R, L_R);
                                                Add_Arg (To_String (F_R));
                                                Add_Arg (To_String (L_R));
                                             end;
                                          end loop;
                                       end if;
                                    end if;
                                 end;
                              elsif FTI.Kind = TK_Record or else FTI.Kind = TK_Constrained_Array then
                                 declare
                                    AV          : Expr_Val;
                                    Is_Rep_Conv : Boolean := False;
                                 begin
                                    if Mode = DN_IN then
                                       AV := Emit_Expr (Ctx, Act, FT);
                                       -- P5-R4: Derived subprogram call on derived record with changed representation (IN)
                                       if FTI.Kind = TK_Record and then Needs_Record_Rep_Conv (Ctx, AV.Ada_Type, FT) then
                                          declare
                                             Tmp_I : constant String := Next_Reg (Ctx.E, "rcin");
                                             Sz_I  : constant Natural := Natural'Max (1, FTI.Byte_Size);
                                             Al_I  : constant Natural := Natural'Max (1, FTI.Align);
                                          begin
                                             Emit_Entry_Alloca (Ctx.E, Tmp_I & " = alloca [" & Img (Sz_I) & " x i8], align " & Img (Al_I));
                                             Emit_Inst (Ctx.E, "memset " & Tmp_I & ", 0, " & Img (Sz_I) & ", align " & Img (Al_I));
                                             Emit_Record_Rep_Copy_Into (Ctx, Tmp_I, FT, To_String (AV.Reg), AV.Ada_Type);
                                             AV.Kind := EK_Addr; AV.Reg := To_Unbounded_String (Tmp_I);
                                             AV.Pir_Type := FTI.Pir_Type; AV.Ada_Type := FT; AV.Align := Al_I;
                                          end;
                                       end if;
                                    elsif FTI.Kind = TK_Record then
                                       -- P3-6.7 / P5-R4: Handle OUT/IN OUT record view conversions AND derived subprogram calls across differing record reps!
                                       declare
                                          In_A : TREE := Act;
                                          In_T : TREE;
                                       begin
                                          while not Is_Void_Tree (In_A) and then KIND (In_A) in DN_CONVERSION | DN_PARENTHESIZED loop
                                             In_A := D (AS_EXP, In_A);
                                          end loop;
                                          In_T := (if not Is_Void_Tree (In_A) then Subst_Tree (Ctx, D (SM_EXP_TYPE, In_A)) else CONST_VOID);
                                          if Needs_Record_Rep_Conv (Ctx, In_T, FT) then
                                             declare
                                                In_LV  : constant Expr_Val := Emit_LValue (Ctx, In_A);
                                                Tmp_R  : constant String := Next_Reg (Ctx.E, "rcot");
                                                Sz_F   : constant Natural := Natural'Max (1, FTI.Byte_Size);
                                                Al_F   : constant Natural := Natural'Max (1, FTI.Align);
                                                Dst_BR : constant TREE := Subst_Base_Type (Ctx, FT);
                                             begin
                                                Is_Rep_Conv := True;
                                                Emit_Entry_Alloca (Ctx.E, Tmp_R & " = alloca [" & Img (Sz_F) & " x i8], align " & Img (Al_F));
                                                Emit_Inst (Ctx.E, "memset " & Tmp_R & ", 0, " & Img (Sz_F) & ", align " & Img (Al_F));
                                                if Mode = DN_IN_OUT then
                                                   Emit_Record_Rep_Copy_Into (Ctx, Tmp_R, FT, To_String (In_LV.Reg), In_T);
                                                else
                                                   -- LRM 6.4.1(4) / P5-R4: copy discriminants AND allocate nested dynamic-array descriptors on OUT!
                                                   Emit_Record_Rep_Copy_Into (Ctx, Tmp_R, FT, To_String (In_LV.Reg), In_T, Discs_Only => True);
                                                end if;
                                                AV.Kind := EK_Addr; AV.Reg := To_Unbounded_String (Tmp_R);
                                                AV.Pir_Type := FTI.Pir_Type; AV.Ada_Type := FT; AV.Align := Al_F;
                                                -- Check constrained conversion types on entry (including intermediate conversions)
                                                declare
                                                   Walk_C : TREE := Act;
                                                begin
                                                   while not Is_Void_Tree (Walk_C) and then KIND (Walk_C) in DN_CONVERSION | DN_PARENTHESIZED loop
                                                      if KIND (Walk_C) = DN_CONVERSION then
                                                         declare
                                                            CT_C : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Walk_C));
                                                         begin
                                                            if not Is_Void_Tree (CT_C) and then KIND (Resolve_Type_Spec (CT_C)) = DN_CONSTRAINED_RECORD then
                                                               if not Needs_Record_Rep_Conv (Ctx, CT_C, FT) then
                                                                  Emit_Discriminant_Check (Ctx, Tmp_R, CT_C, Is_Access => False);
                                                               elsif not Needs_Record_Rep_Conv (Ctx, CT_C, In_T) then
                                                                  Emit_Discriminant_Check (Ctx, To_String (In_LV.Reg), CT_C, Is_Access => False);
                                                               end if;
                                                            end if;
                                                         end;
                                                      end if;
                                                      Walk_C := D (AS_EXP, Walk_C);
                                                   end loop;
                                                end;
                                                pragma Unreferenced (Dst_BR);
                                                Rec_Convs.Append ((
                                                  Tmp_Ptr        => AV.Reg,
                                                  Dst_Ptr        => In_LV.Reg,
                                                  Src_T          => FT,
                                                  Dst_T          => In_T,
                                                  Check_Dst_Disc => (not Is_Void_Tree (In_T) and then KIND (Resolve_Type_Spec (In_T)) = DN_CONSTRAINED_RECORD)));
                                             end;
                                          else
                                             AV := Emit_LValue (Ctx, Act);
                                          end if;
                                       end;
                                    else
                                       AV := Emit_LValue (Ctx, Act);
                                    end if;
                                    if FTI.Kind = TK_Record then
                                       Emit_Discriminant_Check (Ctx, To_String (AV.Reg), FT, Is_Access => False);
                                       if not Is_Void_Tree (DFT) then
                                          if not Needs_Record_Rep_Conv (Ctx, DFT, FT) then
                                             Emit_Discriminant_Check (Ctx, To_String (AV.Reg), DFT, Is_Access => False);
                                          end if;
                                       end if;
                                       -- P3-3.4 / P5-R4: On record OUT/IN OUT view conversion, verify discriminants across ALL constrained conversions on return!
                                       if (Mode = DN_OUT or else Mode = DN_IN_OUT) and then KIND (Act) = DN_CONVERSION then
                                          declare
                                             Walk_A : TREE := Act;
                                          begin
                                             while not Is_Void_Tree (Walk_A) and then KIND (Walk_A) in DN_CONVERSION | DN_PARENTHESIZED loop
                                                if KIND (Walk_A) = DN_CONVERSION then
                                                   declare
                                                      Conv_RT : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Walk_A));
                                                   begin
                                                      if not Is_Void_Tree (Conv_RT)
                                                        and then KIND (Resolve_Type_Spec (Conv_RT)) = DN_CONSTRAINED_RECORD
                                                        and then not Needs_Record_Rep_Conv (Ctx, Conv_RT, FT)
                                                      then
                                                         Post_Checks.Append ((Act_Ptr => AV.Reg, Act_Type => Conv_RT, Is_Record => True));
                                                      end if;
                                                   end;
                                                end if;
                                                Walk_A := D (AS_EXP, Walk_A);
                                             end loop;
                                             if not Is_Rep_Conv and then not Is_Void_Tree (Walk_A) then
                                                declare
                                                   In_T : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Walk_A));
                                                begin
                                                   if not Is_Void_Tree (In_T)
                                                     and then KIND (Resolve_Type_Spec (In_T)) = DN_CONSTRAINED_RECORD
                                                   then
                                                      Post_Checks.Append ((Act_Ptr => AV.Reg, Act_Type => In_T, Is_Record => True));
                                                   end if;
                                                end;
                                             end if;
                                          end;
                                       end if;
                                    else
                                       declare
                                          Tgt : Expr_Val;
                                          ND_A : constant Natural := Natural'Max (1, Natural'Min (FTI.NDims, Max_Array_Dims));
                                          Act_U : TREE := Act;
                                       begin
                                          while not Is_Void_Tree (Act_U) and then KIND (Act_U) = DN_PARENTHESIZED loop
                                             Act_U := D (AS_EXP, Act_U);
                                          end loop;
                                          Tgt.Kind := EK_Slice; Tgt.Ada_Type := FT; Tgt.NDims := ND_A;
                                          for D_Idx in 1 .. ND_A loop
                                             Emit_Index_Bounds (Ctx, FTI.Dims (D_Idx).Idx_Type, Tgt.First_Regs (D_Idx), Tgt.Last_Regs (D_Idx));
                                          end loop;
                                          if not Is_Void_Tree (Act_U)
                                            and then (KIND (Act_U) = DN_STRING_LITERAL
                                                      or else (KIND (Act_U) = DN_AGGREGATE and then not Is_Named_No_Others_Agg (Act_U, ND_A)))
                                          then
                                             Emit_Array_Length_Check (Ctx, Tgt, AV, ND_A);
                                          elsif Ctx.Enable_Checks then
                                             declare
                                                AV_W : constant Array_View := Resolve_Array_View (Ctx, AV);
                                             begin
                                                Emit_Exact_Bounds_Check (Ctx, AV_W.Firsts, AV_W.Lasts, Tgt.First_Regs, Tgt.Last_Regs, ND_A);
                                             end;
                                          end if;
                                       end;
                                    end if;
                                    Add_Arg (To_String (AV.Reg));
                                    if not SI.Is_C_Interface and then Needs_Constrained_Param (Ctx, FT, Mode) then
                                       Add_Arg (Emit_Actual_Constrained (Ctx, Act));
                                    end if;
                                 end;
                              elsif Mode = DN_OUT or else Mode = DN_IN_OUT then
                                 if KIND (Act) = DN_CONVERSION then
                                    -- LRM 6.4.1(3) (P3-3.4, P4-4): view conversion on scalar/access OUT or IN OUT actual (including bitfields & nested chains!)
                                    declare
                                       Inner_N    : TREE := Act;
                                       Conv_Chain : Conv_Chain_Arr := (others => CONST_VOID);
                                       N_Convs    : Natural := 0;
                                       Tmp_Slot   : constant String := Next_Reg (Ctx.E, "vct");
                                       BF_Rec     : Unbounded_String;
                                       BF_FE      : Field_Entry;
                                       BF_CT      : TREE := CONST_VOID;
                                       Is_BF      : Boolean := False;
                                       Inner_LV   : Expr_Val;
                                       Dst_P      : Unbounded_String;
                                       Inner_T    : TREE;
                                       ITI        : Type_Info;
                                    begin
                                       -- Capture every DN_CONVERSION in the chain (1 = outermost .. N_Convs = innermost)
                                       while not Is_Void_Tree (Inner_N) and then KIND (Inner_N) = DN_CONVERSION loop
                                          if N_Convs < Conv_Chain'Last then
                                             N_Convs := N_Convs + 1;
                                             Conv_Chain (N_Convs) := Subst_Tree (Ctx, D (SM_EXP_TYPE, Inner_N));
                                          end if;
                                          Inner_N := D (AS_EXP, Inner_N);
                                       end loop;
                                       Is_BF := Try_Get_Bitfield_Target (Inner_N, BF_Rec, BF_FE, BF_CT);
                                       if Is_BF then
                                          Dst_P   := BF_Rec;
                                          Inner_T := BF_CT;
                                       else
                                          Inner_LV := Emit_LValue (Ctx, Inner_N);
                                          Dst_P    := Inner_LV.Reg;
                                          Inner_T  := Inner_LV.Ada_Type;
                                       end if;
                                       ITI := Get_Type_Info (Ctx, Inner_T);
                                       Emit_Entry_Alloca (Ctx.E, Tmp_Slot & " = alloca " & To_String (FTI.Pir_Type) & ", align " & Img (Natural'Max (1, FTI.Align)));
                                       if Mode = DN_IN_OUT then
                                          declare
                                             Cur_Reg : Unbounded_String;
                                             Cur_T   : TREE := Inner_T;
                                             Cur_Pir : Unbounded_String := ITI.Pir_Type;
                                          begin
                                             if Is_BF then
                                                Cur_Reg := To_Unbounded_String (Emit_Bitfield_Load (Ctx, To_String (Dst_P), BF_FE, Inner_T));
                                             else
                                                declare
                                                   R_Ld : constant String := Next_Reg (Ctx.E, "vci");
                                                begin
                                                   if Is_Shared_Object (Ctx, Inner_N) then
                                                      Emit_Inst (Ctx.E, R_Ld & " = load." & To_String (ITI.Pir_Type) & " " & To_String (Dst_P) & ", align " & Img (Natural'Max (1, ITI.Align)) & " [volatile]");
                                                      Emit_Inst (Ctx.E, "fence [seq_cst]");
                                                   else
                                                      Emit_Inst (Ctx.E, R_Ld & " = load." & To_String (ITI.Pir_Type) & " " & To_String (Dst_P) & ", align " & Img (Natural'Max (1, ITI.Align)));
                                                   end if;
                                                   Cur_Reg := To_Unbounded_String (R_Ld);
                                                end;
                                             end if;
                                             for CI in reverse 1 .. N_Convs loop
                                                declare
                                                   Tgt_T  : constant TREE := Conv_Chain (CI);
                                                   Tgt_TI : constant Type_Info := Get_Type_Info (Ctx, Tgt_T);
                                                   Tgt_P  : constant String := To_String (Tgt_TI.Pir_Type);
                                                   Next_R : constant String :=
                                                     Emit_Typed_Scalar_Convert (Ctx, To_String (Cur_Reg), Cur_T, To_String (Cur_Pir), Tgt_T, Tgt_P, Check_Range => True);
                                                begin
                                                   Emit_Discriminant_Check (Ctx, Next_R, Tgt_T, Is_Access => True);
                                                   Cur_Reg := To_Unbounded_String (Next_R);
                                                   Cur_T   := Tgt_T;
                                                   Cur_Pir := To_Unbounded_String (Tgt_P);
                                                end;
                                             end loop;
                                             if not Is_Void_Tree (DFT) then
                                                declare
                                                   DFTI : constant Type_Info := Get_Type_Info (Ctx, DFT);
                                                   DF_R : constant String :=
                                                     Emit_Typed_Scalar_Convert (Ctx, To_String (Cur_Reg), Cur_T, To_String (Cur_Pir), DFT, To_String (DFTI.Pir_Type), Check_Range => True);
                                                begin
                                                   Emit_Discriminant_Check (Ctx, DF_R, DFT, Is_Access => True);
                                                   Cur_Reg := To_Unbounded_String (DF_R);
                                                   Cur_T   := DFT;
                                                   Cur_Pir := DFTI.Pir_Type;
                                                end;
                                             end if;
                                             Cur_Reg := To_Unbounded_String
                                               (Emit_Typed_Scalar_Convert (Ctx, To_String (Cur_Reg), Cur_T, To_String (Cur_Pir), FT, To_String (FTI.Pir_Type), Check_Range => True));
                                             Emit_Discriminant_Check (Ctx, To_String (Cur_Reg), FT, Is_Access => True);
                                             Emit_Inst (Ctx.E, "store." & To_String (FTI.Pir_Type) & " " & To_String (Cur_Reg) & ", " & Tmp_Slot & ", align " & Img (Natural'Max (1, FTI.Align)));
                                          end;
                                       else
                                          Emit_Inst (Ctx.E, "memset " & Tmp_Slot & ", 0, " & Img (Natural'Max (1, FTI.Byte_Size)) & ", align 1");
                                       end if;
                                       Conv_Outs.Append ((
                                         Tmp_Ptr    => To_Unbounded_String (Tmp_Slot),
                                         Dst_Ptr    => Dst_P,
                                         Src_Pir    => FTI.Pir_Type,
                                         Dst_Pir    => ITI.Pir_Type,
                                         Src_Align  => Natural'Max (1, FTI.Align),
                                         Dst_Align  => Natural'Max (1, ITI.Align),
                                         Conv_Chain => Conv_Chain,
                                         N_Convs    => N_Convs,
                                         Deriv_FT   => DFT,
                                         Src_Type   => FT,
                                         Dst_Type   => Inner_T,
                                         Is_BF      => Is_BF,
                                         BF_FE      => BF_FE,
                                         Is_Shared  => Is_Shared_Object (Ctx, Inner_N)));
                                       Add_Arg (Tmp_Slot);
                                    end;
                                 else
                                    -- LRM 6.4.1(4): scalar/access OUT or IN OUT copy-in / copy-out range & constraint checks (including bitfields!)
                                    declare
                                       BF_Rec   : Unbounded_String;
                                       BF_FE    : Field_Entry;
                                       BF_CT    : TREE := CONST_VOID;
                                       Is_BF    : constant Boolean := Try_Get_Bitfield_Target (Act, BF_Rec, BF_FE, BF_CT);
                                       LV       : Expr_Val;
                                       Dst_P    : Unbounded_String;
                                       Act_T    : TREE;
                                       ITI      : Type_Info;
                                       Tmp_Slot : constant String := Next_Reg (Ctx.E, "iot");
                                       Act_Sh   : constant Boolean := Is_Shared_Object (Ctx, Act);
                                    begin
                                       if Is_BF then
                                          Dst_P := BF_Rec;
                                          Act_T := BF_CT;
                                       else
                                          LV    := Emit_LValue (Ctx, Act);
                                          Dst_P := LV.Reg;
                                          Act_T := LV.Ada_Type;
                                       end if;
                                       ITI := Get_Type_Info (Ctx, Act_T);
                                       Emit_Entry_Alloca (Ctx.E, Tmp_Slot & " = alloca " & To_String (FTI.Pir_Type) & ", align " & Img (Natural'Max (1, FTI.Align)));
                                       if Mode = DN_IN_OUT then
                                          declare
                                             Ld : Unbounded_String;
                                          begin
                                             if Is_BF then
                                                Ld := To_Unbounded_String (Emit_Bitfield_Load (Ctx, To_String (Dst_P), BF_FE, Act_T));
                                             else
                                                declare
                                                   R_Ld : constant String := Next_Reg (Ctx.E, "ioi");
                                                begin
                                                   if Act_Sh then
                                                      Emit_Inst (Ctx.E, R_Ld & " = load." & To_String (ITI.Pir_Type) & " " & To_String (Dst_P) & ", align " & Img (Natural'Max (1, ITI.Align)) & " [volatile]");
                                                      Emit_Inst (Ctx.E, "fence [seq_cst]");
                                                   else
                                                      Emit_Inst (Ctx.E, R_Ld & " = load." & To_String (ITI.Pir_Type) & " " & To_String (Dst_P) & ", align " & Img (Natural'Max (1, ITI.Align)));
                                                   end if;
                                                   Ld := To_Unbounded_String (R_Ld);
                                                end;
                                             end if;
                                             declare
                                                C_Reg : constant String := Emit_Typed_Scalar_Convert (Ctx, To_String (Ld), Act_T, To_String (ITI.Pir_Type), FT, To_String (FTI.Pir_Type));
                                             begin
                                                if not Is_Void_Tree (DFT) then
                                                   Emit_Range_Check (Ctx, C_Reg, To_String (FTI.Pir_Type), DFT);
                                                   Emit_Discriminant_Check (Ctx, C_Reg, DFT, Is_Access => True);
                                                end if;
                                                Emit_Range_Check (Ctx, C_Reg, To_String (FTI.Pir_Type), FT);
                                                Emit_Discriminant_Check (Ctx, C_Reg, FT, Is_Access => True);
                                                Emit_Inst (Ctx.E, "store." & To_String (FTI.Pir_Type) & " " & C_Reg & ", " & Tmp_Slot & ", align " & Img (Natural'Max (1, FTI.Align)));
                                             end;
                                          end;
                                       else
                                          Emit_Inst (Ctx.E, "memset " & Tmp_Slot & ", 0, " & Img (Natural'Max (1, FTI.Byte_Size)) & ", align 1");
                                       end if;
                                       Conv_Outs.Append ((
                                         Tmp_Ptr    => To_Unbounded_String (Tmp_Slot),
                                         Dst_Ptr    => Dst_P,
                                         Src_Pir    => FTI.Pir_Type,
                                         Dst_Pir    => ITI.Pir_Type,
                                         Src_Align  => Natural'Max (1, FTI.Align),
                                         Dst_Align  => Natural'Max (1, ITI.Align),
                                         Conv_Chain => (others => CONST_VOID),
                                         N_Convs    => 0,
                                         Deriv_FT   => DFT,
                                         Src_Type   => FT,
                                         Dst_Type   => Act_T,
                                         Is_BF      => Is_BF,
                                         BF_FE      => BF_FE,
                                         Is_Shared  => Act_Sh));
                                       Add_Arg (Tmp_Slot);
                                    end;
                                 end if;
                              else
                                 declare
                                    AV : constant Expr_Val := Emit_Expr (Ctx, Act, FT);
                                    SP : constant String := To_String (AV.Pir_Type);
                                    DP : constant String := To_String (FTI.Pir_Type);
                                    Reg_S : Unbounded_String;
                                 begin
                                    -- P3-3.11: Range-check in wider source width BEFORE truncating to narrow formal!
                                    if Int_Bits_Of_Pir (SP) > Int_Bits_Of_Pir (DP) and then Is_Discrete_Type (FT) then
                                       if not Is_Void_Tree (DFT) then
                                          Emit_Range_Check (Ctx, To_String (AV.Reg), SP, DFT);
                                       end if;
                                       Emit_Range_Check (Ctx, To_String (AV.Reg), SP, FT);
                                    end if;
                                    Reg_S := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (AV.Reg), SP, DP,
                                      Is_Unsigned => Is_Unsigned_Scalar (AV.Ada_Type, SP)));
                                    if not Is_Void_Tree (DFT) then
                                       Emit_Range_Check (Ctx, To_String (Reg_S), DP, DFT);
                                       Emit_Discriminant_Check (Ctx, To_String (Reg_S), DFT, Is_Access => True);
                                    end if;
                                    Emit_Range_Check (Ctx, To_String (Reg_S), DP, FT);
                                    Emit_Discriminant_Check (Ctx, To_String (Reg_S), FT, Is_Access => True);
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
                  if SI.Is_C_Interface and then SI.Is_Function and then SI.Ret_Composite then
                     Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args_Buf), "ptr", RV);
                     Ret_Buf := RV;
                  else
                     Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args_Buf), "", RV);
                  end if;
                  for PC of Post_Checks loop
                     if PC.Is_Record then
                        Emit_Discriminant_Check (Ctx, To_String (PC.Act_Ptr), PC.Act_Type, Is_Access => False);
                     else
                        declare
                           AP : constant String := Next_Reg (Ctx.E, "acout");
                        begin
                           Emit_Inst (Ctx.E, AP & " = load.ptr " & To_String (PC.Act_Ptr) & ", align 8");
                           Emit_Discriminant_Check (Ctx, AP, PC.Act_Type, Is_Access => True);
                        end;
                     end if;
                  end loop;
                  for RC of Rec_Convs loop
                     if RC.Check_Dst_Disc then
                        declare
                           DTI   : constant Type_Info := Get_Type_Info (Ctx, RC.Dst_T);
                           Sz_D  : constant Natural := Natural'Max (1, DTI.Byte_Size);
                           Al_D  : constant Natural := Natural'Max (1, DTI.Align);
                           Tmp_D : constant String := Next_Reg (Ctx.E, "rccd");
                        begin
                           Emit_Entry_Alloca (Ctx.E, Tmp_D & " = alloca [" & Img (Sz_D) & " x i8], align " & Img (Al_D));
                           Emit_Record_Rep_Copy_Into (Ctx, Tmp_D, RC.Dst_T, To_String (RC.Tmp_Ptr), RC.Src_T);
                           Emit_Discriminant_Check (Ctx, Tmp_D, RC.Dst_T, Is_Access => False);
                           Emit_Inst (Ctx.E, "memcpy " & To_String (RC.Dst_Ptr) & ", " & Tmp_D & ", " & Img (Sz_D) & ", align " & Img (Al_D));
                        end;
                     else
                        Emit_Record_Rep_Copy_Into (Ctx, To_String (RC.Dst_Ptr), RC.Dst_T, To_String (RC.Tmp_Ptr), RC.Src_T);
                     end if;
                  end loop;
                  for CO of Conv_Outs loop
                     declare
                        Lv      : constant String := Next_Reg (Ctx.E, "vco");
                        Cur_Reg : Unbounded_String := To_Unbounded_String (Lv);
                        Cur_T   : TREE := CO.Src_Type;
                        Cur_Pir : Unbounded_String := CO.Src_Pir;
                        Cv      : Unbounded_String;
                     begin
                        Emit_Inst (Ctx.E, Lv & " = load." & To_String (CO.Src_Pir) & " " & To_String (CO.Tmp_Ptr) & ", align " & Img (CO.Src_Align));
                        if not Is_Void_Tree (CO.Deriv_FT) then
                           Emit_Range_Check (Ctx, Lv, To_String (CO.Src_Pir), CO.Deriv_FT);
                           Emit_Discriminant_Check (Ctx, Lv, CO.Deriv_FT, Is_Access => True);
                        end if;
                        for CI in 1 .. CO.N_Convs loop
                           declare
                              CT  : constant TREE := CO.Conv_Chain (CI);
                              CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                              CP  : constant String := To_String (CTI.Pir_Type);
                              T1  : constant String := Emit_Typed_Scalar_Convert (Ctx, To_String (Cur_Reg), Cur_T, To_String (Cur_Pir), CT, CP);
                           begin
                              Emit_Range_Check (Ctx, T1, CP, CT);
                              Emit_Discriminant_Check (Ctx, T1, CT, Is_Access => True);
                              Cur_Reg := To_Unbounded_String (T1);
                              Cur_T   := CT;
                              Cur_Pir := To_Unbounded_String (CP);
                           end;
                        end loop;
                        Cv := To_Unbounded_String (Emit_Typed_Scalar_Convert (Ctx, To_String (Cur_Reg), Cur_T, To_String (Cur_Pir), CO.Dst_Type, To_String (CO.Dst_Pir)));
                        Emit_Range_Check (Ctx, To_String (Cv), To_String (CO.Dst_Pir), CO.Dst_Type);
                        Emit_Discriminant_Check (Ctx, To_String (Cv), CO.Dst_Type, Is_Access => True);
                        if CO.Is_BF then
                           Emit_Bitfield_Store (Ctx, To_String (CO.Dst_Ptr), CO.BF_FE, To_String (Cv), To_String (CO.Dst_Pir), CO.Dst_Type);
                        elsif CO.Is_Shared then
                           Emit_Inst (Ctx.E, "fence [seq_cst]");
                           Emit_Inst (Ctx.E, "store." & To_String (CO.Dst_Pir) & " " & To_String (Cv) & ", " & To_String (CO.Dst_Ptr) & ", align " & Img (CO.Dst_Align) & " [volatile]");
                           Emit_Inst (Ctx.E, "fence [seq_cst]");
                        else
                           Emit_Inst (Ctx.E, "store." & To_String (CO.Dst_Pir) & " " & To_String (Cv) & ", " & To_String (CO.Dst_Ptr) & ", align " & Img (CO.Dst_Align));
                        end if;
                     end;
                  end loop;
                  if SI.Ret_Uncon_Arr then
                     declare
                        RTI     : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                        ND      : constant Natural := (if RTI.NDims > 0 then RTI.NDims else 1);
                        Desc_Ty : constant String := Get_Arr_Desc_Type (Ctx, ND);
                     begin
                        Emit_Unpack_Arr_Desc (Ctx, To_String (Ret_Buf), SI.Ret_Type, Res, Desc_Ty => Desc_Ty);
                        Res.Ada_Type := (if not Is_Void_Tree (Orig_Derived_Ret) then Orig_Derived_Ret else SI.Ret_Type);
                        Res.NDims    := ND;
                        if not Is_Void_Tree (Orig_Derived_Ret) and then Ctx.Enable_Checks then
                           declare
                              DRTI : constant Type_Info := Get_Type_Info (Ctx, Orig_Derived_Ret);
                           begin
                              if DRTI.Kind = TK_Constrained_Array then
                                 declare
                                    DF, DL : Str_Array_4;
                                    ND_R   : constant Natural := Natural'Min (DRTI.NDims, ND);
                                 begin
                                    for D_Idx in 1 .. ND_R loop
                                       Emit_Index_Bounds (Ctx, DRTI.Dims (D_Idx).Idx_Type, DF (D_Idx), DL (D_Idx));
                                    end loop;
                                    Emit_Exact_Bounds_Check (Ctx, Res.First_Regs, Res.Last_Regs, DF, DL, ND_R);
                                 end;
                              end if;
                           end;
                        end if;
                     end;
                  elsif SI.Ret_Composite then
                     declare
                        RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                     begin
                        if not Is_Void_Tree (Orig_Derived_Ret) and then RTI.Kind = TK_Record then
                           if Needs_Record_Rep_Conv (Ctx, SI.Ret_Type, Orig_Derived_Ret) then
                              declare
                                 DRTI : constant Type_Info := Get_Type_Info (Ctx, Orig_Derived_Ret);
                                 CBuf : constant String := Next_Reg (Ctx.E, "rcret");
                                 Sz_R : constant Natural := Natural'Max (1, DRTI.Byte_Size);
                                 Al_R : constant Natural := Natural'Max (1, DRTI.Align);
                              begin
                                 Emit_Entry_Alloca (Ctx.E, CBuf & " = alloca [" & Img (Sz_R) & " x i8], align " & Img (Al_R));
                                 Emit_Inst (Ctx.E, "memset " & CBuf & ", 0, " & Img (Sz_R) & ", align " & Img (Al_R));
                                 Emit_Record_Rep_Copy_Into (Ctx, CBuf, Orig_Derived_Ret, To_String (Ret_Buf), SI.Ret_Type);
                                 Emit_Discriminant_Check (Ctx, CBuf, Orig_Derived_Ret, Is_Access => False);
                                 Res.Kind     := EK_Addr;
                                 Res.Reg      := To_Unbounded_String (CBuf);
                                 Res.Pir_Type := DRTI.Pir_Type;
                                 Res.Ada_Type := Orig_Derived_Ret;
                                 Res.Align    := Al_R;
                              end;
                           else
                              Emit_Discriminant_Check (Ctx, To_String (Ret_Buf), Orig_Derived_Ret, Is_Access => False);
                              Res.Kind     := EK_Addr;
                              Res.Reg      := Ret_Buf;
                              Res.Pir_Type := RTI.Pir_Type;
                              Res.Ada_Type := Orig_Derived_Ret;
                              Res.Align    := RTI.Align;
                           end if;
                        else
                           Res.Kind     := EK_Addr;
                           Res.Reg      := Ret_Buf;
                           Res.Pir_Type := RTI.Pir_Type;
                           Res.Ada_Type := (if not Is_Void_Tree (Orig_Derived_Ret) then Orig_Derived_Ret else SI.Ret_Type);
                           Res.Align    := RTI.Align;
                        end if;
                     end;
                  end if;
               else
                  declare
                     RTI : constant Type_Info := Get_Type_Info (Ctx, SI.Ret_Type);
                  begin
                     Emit_Call_Or_Invoke (Ctx, To_String (SI.Pir_Name), To_String (Args_Buf), To_String (RTI.Pir_Type), RV);
                     for PC of Post_Checks loop
                        if PC.Is_Record then
                           Emit_Discriminant_Check (Ctx, To_String (PC.Act_Ptr), PC.Act_Type, Is_Access => False);
                        else
                           declare
                              AP : constant String := Next_Reg (Ctx.E, "acout");
                           begin
                              Emit_Inst (Ctx.E, AP & " = load.ptr " & To_String (PC.Act_Ptr) & ", align 8");
                              Emit_Discriminant_Check (Ctx, AP, PC.Act_Type, Is_Access => True);
                           end;
                        end if;
                     end loop;
                     if Invert_Bool then
                        declare
                           Inv : constant String := Next_Reg (Ctx.E, "ineq");
                        begin
                           Emit_Inst (Ctx.E, Inv & " = xor.i8 " & To_String (RV) & ", 1");
                           RV := To_Unbounded_String (Inv);
                        end;
                     elsif not Is_Void_Tree (Orig_Derived_Ret) then
                        Emit_Range_Check (Ctx, To_String (RV), To_String (RTI.Pir_Type), Orig_Derived_Ret);
                        Emit_Discriminant_Check (Ctx, To_String (RV), Orig_Derived_Ret, Is_Access => True);
                     end if;
                     Res.Kind     := EK_Scalar;
                     Res.Reg      := RV;
                     Res.Pir_Type := RTI.Pir_Type;
                     Res.Ada_Type := (if Invert_Bool then Res_Type
                                      elsif not Is_Void_Tree (Orig_Derived_Ret) then Orig_Derived_Ret
                                      else SI.Ret_Type);
                     Res.Align    := RTI.Align;
                  end;
               end if;
            end;
            end;
            return Res;
         end;
      end;
   end Emit_Subp_Call;

end Cg_Expr.Call;
