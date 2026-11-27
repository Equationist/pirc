-- Copyright (c) 2026 Ummon Karpe
-- SPDX-License-Identifier: BSD-3-Clause

with DIANA; use DIANA;
with DIANATTR; use DIANATTR;
with Diana_Util; use Diana_Util;
with Pir_Emit; use Pir_Emit;

package body Cg_Expr.RTL is

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
                  if Gen_N = "UNCHECKED_DEALLOCATION" and then Is_Library_Level (Gen_Def) then
                     Handled := True;
                     declare
                        Arg_N : TREE := Args (1);
                     begin
                        while not Is_Void_Tree (Arg_N) and then KIND (Arg_N) = DN_CONVERSION loop
                           Arg_N := D (AS_EXP, Arg_N);
                        end loop;
                        declare
                        LV    : constant Expr_Val := Emit_LValue (Ctx, Arg_N);
                        Acc_T : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, LV.Ada_Type));
                        Des_T : constant TREE :=
                          (if not Is_Void_Tree (Acc_T) and then KIND (Acc_T) in DN_ACCESS | DN_CONSTRAINED_ACCESS
                           then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Acc_T))) else CONST_VOID);
                        Pool_Base : TREE := Base_Type_Spec (Acc_T);
                        Has_Pool  : Boolean := False;
                        PV    : constant String := Next_Reg (Ctx.E, "fp");
                        Dummy : Unbounded_String;
                     begin
                        declare
                           Cur : TREE := Pool_Base;
                        begin
                           for Step in 1 .. 16 loop
                              exit when Is_Void_Tree (Cur) or else KIND (Cur) /= DN_ACCESS;
                              if not Is_Void_Tree (D (SM_STORAGE_SIZE, Cur)) then
                                 Pool_Base := Cur;
                                 Has_Pool  := True;
                                 exit;
                              end if;
                              Cur := Resolve_Type_Spec (D (SM_DERIVED, Cur));
                           end loop;
                        end;
                        Emit_Inst (Ctx.E, PV & " = load.ptr " & To_String (LV.Reg) & ", align 8");
                        if not Is_Void_Tree (Des_T) and then Is_Task_Type (Des_T) then
                           -- LRM 13.10.1 / cda101b: task continues executing; only null out the access variable
                           null;
                        elsif Has_Pool then
                           declare
                              Cur_N : constant TREE := D (XD_SOURCE_NAME, Pool_Base);
                              Is_Lib_Pool : constant Boolean :=
                                not Is_Void_Tree (Cur_N) and then Is_Library_Level (Cur_N);
                              PSym : constant String :=
                                (if Is_Lib_Pool then "@ada_pool_used__" & Qualified_Ada_Name (Cur_N)
                                 else "@ada_pool_used_" & Tree_Key_Str (Pool_Base) & To_String (Ctx.Gen_Inst_Tag));
                              UP   : constant String := Next_Reg (Ctx.E, "pup");
                           begin
                              if Is_Lib_Pool then
                                 Add_Global_Decl (Ctx.E, PSym, "extern " & PSym & " = global i64, align 8");
                              else
                                 Add_Global_Decl (Ctx.E, PSym, PSym & " = global i64 = 0, align 8");
                              end if;
                              Emit_Inst (Ctx.E, UP & " = addr " & PSym);
                              if Is_Access_To_Uncon_Array (Acc_T) then
                                 declare
                                    L_Nn : constant String := Next_Label (Ctx.E, "pnn");
                                    L_Dn : constant String := Next_Label (Ctx.E, "pdn");
                                    Cnd  : constant String := Next_Reg (Ctx.E, "pnc");
                                    P0   : constant String := Next_Reg (Ctx.E, "pp0");
                                    DP   : constant String := Next_Reg (Ctx.E, "pdp");
                                 begin
                                    Emit_Inst (Ctx.E, Cnd & " = cmp_ne.ptr " & PV & ", 0");
                                    Emit_Term (Ctx.E, "br " & Cnd & ", " & L_Nn & ", " & L_Dn);
                                    Emit_Label (Ctx.E, L_Nn);
                                    Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & PV & ", 0");
                                    Emit_Inst (Ctx.E, DP & " = load.ptr " & P0 & ", align 8");
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_pool_free", UP & ", " & DP, "", Dummy, Nounwind => True);
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_free", PV, "", Dummy, Nounwind => True);
                                    Emit_Term (Ctx.E, "jmp " & L_Dn);
                                    Emit_Label (Ctx.E, L_Dn);
                                 end;
                              else
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_pool_free", UP & ", " & PV, "", Dummy, Nounwind => True);
                              end if;
                           end;
                        elsif Is_Access_To_Uncon_Array (Acc_T) then
                           declare
                              L_Nn : constant String := Next_Label (Ctx.E, "unn");
                              L_Dn : constant String := Next_Label (Ctx.E, "udn");
                              Cnd  : constant String := Next_Reg (Ctx.E, "unc");
                              P0   : constant String := Next_Reg (Ctx.E, "up0");
                              DP   : constant String := Next_Reg (Ctx.E, "udp");
                           begin
                              Emit_Inst (Ctx.E, Cnd & " = cmp_ne.ptr " & PV & ", 0");
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_Nn & ", " & L_Dn);
                              Emit_Label (Ctx.E, L_Nn);
                              Emit_Inst (Ctx.E, P0 & " = field_ptr @__ada_arr_desc, " & PV & ", 0");
                              Emit_Inst (Ctx.E, DP & " = load.ptr " & P0 & ", align 8");
                              Emit_Call_Or_Invoke (Ctx, "@__ada_free", DP, "", Dummy, Nounwind => True);
                              Emit_Call_Or_Invoke (Ctx, "@__ada_free", PV, "", Dummy, Nounwind => True);
                              Emit_Term (Ctx.E, "jmp " & L_Dn);
                              Emit_Label (Ctx.E, L_Dn);
                           end;
                        else
                           Emit_Call_Or_Invoke (Ctx, "@__ada_free", PV, "", Dummy, Nounwind => True);
                        end if;
                        Emit_Inst (Ctx.E, "store.ptr 0, " & To_String (LV.Reg) & ", align 8");
                        end;
                     end;
                     return Res;
                  elsif Gen_N = "UNCHECKED_CONVERSION" and then Is_Library_Level (Gen_Def) then
                     Handled := True;
                     declare
                        Src : constant Expr_Val := Emit_Expr (Ctx, Args (1));
                        STI : constant Type_Info := Get_Type_Info (Ctx, Src.Ada_Type);
                        RTI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
                        R   : constant String := Next_Reg (Ctx.E, "uc");

                        function Is_Bool_Bit_Array (ATi : Type_Info) return Boolean is
                        begin
                           if (ATi.Kind = TK_Constrained_Array or else ATi.Kind = TK_Unconstrained_Array)
                             and then ATi.NDims = 1
                             and then not Is_Void_Tree (ATi.Elem_Type)
                             and then KIND (Base_Type_Spec (ATi.Elem_Type)) = DN_ENUMERATION
                             and then Sym_Name (D (XD_SOURCE_NAME, Base_Type_Spec (ATi.Elem_Type))) = "BOOLEAN"
                           then
                              return True;
                           end if;
                           return False;
                        end Is_Bool_Bit_Array;
                     begin
                        if RTI.Kind /= TK_Scalar and then STI.Kind = TK_Scalar
                          and then Is_Bool_Bit_Array (RTI)
                        then
                           -- cda201a/b, cd2a31c (LENGTH_CHECK): unpack scalar bits into 1D BOOLEAN array (static or dynamic!)
                           declare
                              F_R, L_R : Unbounded_String;
                              NB64 : Unbounded_String;
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              IV  : Unbounded_String;
                              L_H : constant String := Next_Label (Ctx.E, "ubu_h");
                              L_B : constant String := Next_Label (Ctx.E, "ubu_b");
                              L_E : constant String := Next_Label (Ctx.E, "ubu_e");
                              IR  : constant String := Next_Reg (Ctx.E, "ubi");
                              Cnd : constant String := Next_Reg (Ctx.E, "ubc");
                              Lt64 : constant String := Next_Reg (Ctx.E, "ubl6");
                              Sh  : constant String := Next_Reg (Ctx.E, "ubs");
                              Bt  : constant String := Next_Reg (Ctx.E, "ubt");
                              Sel : constant String := Next_Reg (Ctx.E, "ubz");
                              B8  : constant String := Next_Reg (Ctx.E, "ub8");
                              EP  : constant String := Next_Reg (Ctx.E, "ubp");
                              Nx  : constant String := Next_Reg (Ctx.E, "ubn");
                           begin
                              if To_String (Src.Pir_Type) = "f32" then
                                 declare
                                    B32 : constant String := Next_Reg (Ctx.E, "ubf");
                                 begin
                                    Emit_Inst (Ctx.E, B32 & " = bitcast.i32 " & To_String (Src.Reg));
                                    IV := To_Unbounded_String (Emit_Int_Convert (Ctx, B32, "i32", "i64", Is_Unsigned => True));
                                 end;
                              elsif To_String (Src.Pir_Type) = "f64" then
                                 declare
                                    B64 : constant String := Next_Reg (Ctx.E, "ubf");
                                 begin
                                    Emit_Inst (Ctx.E, B64 & " = bitcast.i64 " & To_String (Src.Reg));
                                    IV := To_Unbounded_String (B64);
                                 end;
                              else
                                 declare
                                    Src_Rep : constant TREE := Find_Enum_Rep_Type (Ctx, Src.Ada_Type);
                                 begin
                                    if not Is_Void_Tree (Src_Rep) then
                                       IV := To_Unbounded_String (Emit_Enum_Pos_To_Rep_I64 (Ctx, Src_Rep, To_String (Src.Reg), To_String (Src.Pir_Type)));
                                    else
                                       IV := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Src.Reg), To_String (Src.Pir_Type), "i64",
                                         Is_Unsigned => Is_Unsigned_Scalar (Src.Ada_Type, To_String (Src.Pir_Type))));
                                    end if;
                                 end;
                              end if;
                              if RTI.Kind = TK_Constrained_Array and then not RTI.Is_Dynamic and then RTI.Dims (1).Is_Static then
                                 declare
                                    Len : constant Integer_64 := (if RTI.Dims (1).Last >= RTI.Dims (1).First then RTI.Dims (1).Last - RTI.Dims (1).First + 1 else 0);
                                    NR  : constant String := Next_Reg (Ctx.E, "ubn0");
                                 begin
                                    Emit_Inst (Ctx.E, NR & " = const.i64 " & Img (Long_Long_Integer (Len)));
                                    NB64 := To_Unbounded_String (NR);
                                    Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, Natural (Len))) & " x i8], align 1");
                                 end;
                              else
                                 Emit_Index_Bounds (Ctx, RTI.Dims (1).Idx_Type, F_R, L_R);
                                 NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_R), To_String (L_R)));
                                 declare
                                    Nz    : constant String := Next_Reg (Ctx.E, "ubnz");
                                    AlB   : constant String := Next_Reg (Ctx.E, "ubal");
                                    Buf_U : Unbounded_String;
                                 begin
                                    Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (NB64) & ", 0");
                                    Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (NB64) & ", 1");
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", AlB & ", 1", "ptr", Buf_U);
                                    Emit_Inst (Ctx.E, Buf & " = ptr_add.i64 " & To_String (Buf_U) & ", 0");
                                 end;
                              end if;
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (NB64));
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, Lt64 & " = cmp_lt_s.i64 " & IR & ", 64");
                              declare
                                 Safe_IR : constant String := Next_Reg (Ctx.E, "ubs6");
                              begin
                                 Emit_Inst (Ctx.E, Safe_IR & " = select.i64 " & Lt64 & ", " & IR & ", 0");
                                 Emit_Inst (Ctx.E, Sh & " = shr.i64 " & To_String (IV) & ", " & Safe_IR);
                              end;
                              Emit_Inst (Ctx.E, Bt & " = and.i64 " & Sh & ", 1");
                              Emit_Inst (Ctx.E, Sel & " = select.i64 " & Lt64 & ", " & Bt & ", 0");
                              Emit_Inst (Ctx.E, B8 & " = trunc.i8 " & Sel);
                              Emit_Inst (Ctx.E, EP & " = elem_ptr i8, " & Buf & ", " & IR);
                              Emit_Inst (Ctx.E, "store.i8 " & B8 & ", " & EP & ", align 1");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              if RTI.Kind = TK_Constrained_Array and then not RTI.Is_Dynamic then
                                 Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := 1;
                              else
                                 Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (Buf);
                                 Res.Pir_Type := To_Unbounded_String ("ptr"); Res.Ada_Type := Res_Type; Res.Align := 1;
                                 Res.NDims := 1; Res.First_Regs (1) := F_R; Res.Last_Regs (1) := L_R;
                              end if;
                           end;
                        elsif RTI.Kind = TK_Scalar and then STI.Kind /= TK_Scalar
                          and then Is_Bool_Bit_Array (STI)
                        then
                           -- cda201a/b, cd2a31c: pack 1D BOOLEAN array (static or dynamic) into scalar (with sign-extension for signed ints)
                           declare
                              SF, SL : Unbounded_String;
                              NB64   : Unbounded_String;
                              Lim64  : constant String := Next_Reg (Ctx.E, "upl");
                              Lt64   : constant String := Next_Reg (Ctx.E, "uplc");
                              Acc : constant String := Next_Reg (Ctx.E, "upa");
                              L_H : constant String := Next_Label (Ctx.E, "ubp_h");
                              L_B : constant String := Next_Label (Ctx.E, "ubp_b");
                              L_E : constant String := Next_Label (Ctx.E, "ubp_e");
                              IR  : constant String := Next_Reg (Ctx.E, "upi");
                              Cnd : constant String := Next_Reg (Ctx.E, "upc");
                              EP  : constant String := Next_Reg (Ctx.E, "upp");
                              EV  : constant String := Next_Reg (Ctx.E, "upv");
                              E64 : constant String := Next_Reg (Ctx.E, "up6");
                              E1  : constant String := Next_Reg (Ctx.E, "up1");
                              Sh  : constant String := Next_Reg (Ctx.E, "ups");
                              Cur : constant String := Next_Reg (Ctx.E, "upk");
                              Nw  : constant String := Next_Reg (Ctx.E, "upw");
                              Nx  : constant String := Next_Reg (Ctx.E, "upn");
                              Fin : Unbounded_String := To_Unbounded_String (Next_Reg (Ctx.E, "upf"));
                           begin
                              Get_Array_Dim_Bounds (Ctx, Src, 1, SF, SL);
                              NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (SF), To_String (SL)));
                              Emit_Inst (Ctx.E, Lt64 & " = cmp_lt_s.i64 " & To_String (NB64) & ", 64");
                              Emit_Inst (Ctx.E, Lim64 & " = select.i64 " & Lt64 & ", " & To_String (NB64) & ", 64");
                              Emit_Entry_Alloca (Ctx.E, Acc & " = alloca i64, align 8");
                              Emit_Inst (Ctx.E, "store.i64 0, " & Acc & ", align 8");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Lim64);
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, EP & " = elem_ptr i8, " & To_String (Src.Reg) & ", " & IR);
                              Emit_Inst (Ctx.E, EV & " = load.i8 " & EP & ", align 1");
                              Emit_Inst (Ctx.E, E64 & " = zext.i64 " & EV);
                              Emit_Inst (Ctx.E, E1 & " = and.i64 " & E64 & ", 1");
                              Emit_Inst (Ctx.E, Sh & " = shl.i64 " & E1 & ", " & IR);
                              Emit_Inst (Ctx.E, Cur & " = load.i64 " & Acc & ", align 8");
                              Emit_Inst (Ctx.E, Nw & " = or.i64 " & Cur & ", " & Sh);
                              Emit_Inst (Ctx.E, "store.i64 " & Nw & ", " & Acc & ", align 8");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              Emit_Inst (Ctx.E, To_String (Fin) & " = load.i64 " & Acc & ", align 8");
                              -- Sign-extend narrow bit-width into signed integer or negative-rep enum types (P3-6.2)
                              declare
                                 Res_Rep : constant TREE := Find_Enum_Rep_Type (Ctx, Res_Type);
                              begin
                              if Is_Integer_Type (Res_Type)
                                or else Enum_Rep_Has_Negative (Res_Rep)
                              then
                                 declare
                                    Need_Sign_Ext : Boolean := False;
                                    St_R : Boolean;
                                    RLo, RHi : Integer_64;
                                 begin
                                    if Enum_Rep_Has_Negative (Res_Rep) then
                                       Need_Sign_Ext := True;
                                    else
                                       Get_Discrete_Bounds (Resolve_Type_Spec (Res_Type), St_R, RLo, RHi);
                                       Need_Sign_Ext := (not St_R) or else RLo < 0;
                                    end if;
                                    if Need_Sign_Ext then
                                       declare
                                          Need_S : constant String := Next_Reg (Ctx.E, "upsn");
                                          ShAmt  : constant String := Next_Reg (Ctx.E, "upsa");
                                          ShL    : constant String := Next_Reg (Ctx.E, "upsl");
                                          ShR    : constant String := Next_Reg (Ctx.E, "upsr");
                                          SFin   : constant String := Next_Reg (Ctx.E, "upsf");
                                          Gt0    : constant String := Next_Reg (Ctx.E, "upg0");
                                          Lt6    : constant String := Next_Reg (Ctx.E, "upl6");
                                          SafeA  : constant String := Next_Reg (Ctx.E, "upsz");
                                       begin
                                          Emit_Inst (Ctx.E, Gt0 & " = cmp_gt_s.i64 " & Lim64 & ", 0");
                                          Emit_Inst (Ctx.E, Lt6 & " = cmp_lt_s.i64 " & Lim64 & ", 64");
                                          Emit_Inst (Ctx.E, Need_S & " = and.i8 " & Gt0 & ", " & Lt6);
                                          Emit_Inst (Ctx.E, ShAmt & " = sub.i64 64, " & Lim64);
                                          Emit_Inst (Ctx.E, SafeA & " = select.i64 " & Need_S & ", " & ShAmt & ", 0");
                                          Emit_Inst (Ctx.E, ShL & " = shl.i64 " & To_String (Fin) & ", " & SafeA);
                                          Emit_Inst (Ctx.E, ShR & " = sar.i64 " & ShL & ", " & SafeA);
                                          Emit_Inst (Ctx.E, SFin & " = select.i64 " & Need_S & ", " & ShR & ", " & To_String (Fin));
                                          Fin := To_Unbounded_String (SFin);
                                       end;
                                    end if;
                                 end;
                              end if;
                              -- LRM 13.4 (cd2a23a/b, cd2a24a): reverse-map custom enum SM_REP -> SM_POS via canonical helper!
                              if not Is_Void_Tree (Res_Rep) then
                                 Fin := To_Unbounded_String (Emit_Enum_Rep_I64_To_Pos (Ctx, Res_Rep, To_String (Fin)));
                              end if;
                              end;
                              if To_String (RTI.Pir_Type) = "f32" then
                                 declare
                                    T32 : constant String := Next_Reg (Ctx.E, "up3");
                                 begin
                                    Emit_Inst (Ctx.E, T32 & " = trunc.i32 " & To_String (Fin));
                                    Emit_Inst (Ctx.E, R & " = bitcast.f32 " & T32);
                                 end;
                              elsif To_String (RTI.Pir_Type) = "f64" then
                                 Emit_Inst (Ctx.E, R & " = bitcast.f64 " & To_String (Fin));
                              else
                                 declare
                                    Cv : constant String := Emit_Int_Convert (Ctx, To_String (Fin), "i64", To_String (RTI.Pir_Type));
                                 begin
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (Cv);
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                                    return Res;
                                 end;
                              end if;
                              Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                           end;
                        elsif Is_Bool_Bit_Array (RTI) and then Is_Bool_Bit_Array (STI) then
                           -- Bool_Arr -> Bool_Arr: both are 1-byte-per-element BOOLEAN arrays; copy directly!
                           declare
                              SF, SL, DF, DL : Unbounded_String;
                              SLen, DLen     : Unbounded_String;
                              Use_L          : constant String := Next_Reg (Ctx.E, "ubml");
                              Lt             : constant String := Next_Reg (Ctx.E, "ublt");
                              Nz             : constant String := Next_Reg (Ctx.E, "ubnz");
                              AlB            : constant String := Next_Reg (Ctx.E, "ubal");
                              Buf            : constant String := Next_Reg (Ctx.E, "ubbf");
                           begin
                              Get_Array_Dim_Bounds (Ctx, Src, 1, SF, SL);
                              SLen := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (SF), To_String (SL)));
                              if RTI.Kind = TK_Constrained_Array and then not RTI.Is_Dynamic and then RTI.Dims (1).Is_Static then
                                 declare
                                    Len : constant Integer_64 := (if RTI.Dims (1).Last >= RTI.Dims (1).First then RTI.Dims (1).Last - RTI.Dims (1).First + 1 else 0);
                                    NR  : constant String := Next_Reg (Ctx.E, "ubdn");
                                 begin
                                    Emit_Inst (Ctx.E, NR & " = const.i64 " & Img (Long_Long_Integer (Len)));
                                    DLen := To_Unbounded_String (NR);
                                    Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, Natural (Len))) & " x i8], align 1");
                                    Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (Natural'Max (1, Natural (Len))) & ", align 1");
                                 end;
                              else
                                 Emit_Index_Bounds (Ctx, RTI.Dims (1).Idx_Type, DF, DL);
                                 DLen := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (DF), To_String (DL)));
                                 Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (DLen) & ", 0");
                                 Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (DLen) & ", 1");
                                 declare
                                    Buf_U : Unbounded_String;
                                 begin
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", AlB & ", 1", "ptr", Buf_U);
                                    Emit_Inst (Ctx.E, Buf & " = ptr_add.i64 " & To_String (Buf_U) & ", 0");
                                 end;
                                 Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & AlB & ", align 1");
                              end if;
                              Emit_Inst (Ctx.E, Lt & " = cmp_lt_s.i64 " & To_String (SLen) & ", " & To_String (DLen));
                              Emit_Inst (Ctx.E, Use_L & " = select.i64 " & Lt & ", " & To_String (SLen) & ", " & To_String (DLen));
                              Emit_Inst (Ctx.E, "memcpy " & Buf & ", " & To_String (Src.Reg) & ", " & Use_L & ", align 1");
                              if RTI.Kind = TK_Constrained_Array and then not RTI.Is_Dynamic then
                                 Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := 1;
                              else
                                 Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (Buf);
                                 Res.Pir_Type := To_Unbounded_String ("ptr"); Res.Ada_Type := Res_Type;
                                 Res.Align := 1; Res.NDims := 1;
                                 Res.First_Regs (1) := DF; Res.Last_Regs (1) := DL;
                              end if;
                           end;
                        elsif Is_Bool_Bit_Array (RTI) and then STI.Kind /= TK_Scalar then
                           -- Composite -> Bool bit-array (cd4061a): unpack source bytes bit-by-bit into 0/1 BOOLEANs
                           declare
                              F_R, L_R : Unbounded_String;
                              NB64     : Unbounded_String;
                              AlB      : constant String := Next_Reg (Ctx.E, "ucab");
                              Nz       : constant String := Next_Reg (Ctx.E, "ucnz");
                              Buf      : constant String := Next_Reg (Ctx.E, "ucbf");
                              L_H      : constant String := Next_Label (Ctx.E, "ucb_h");
                              L_B      : constant String := Next_Label (Ctx.E, "ucb_b");
                              L_E      : constant String := Next_Label (Ctx.E, "ucb_e");
                              IR       : constant String := Next_Reg (Ctx.E, "ucbi");
                              Cnd      : constant String := Next_Reg (Ctx.E, "ucbc");
                              ByI      : constant String := Next_Reg (Ctx.E, "ucby");
                              BiI      : constant String := Next_Reg (Ctx.E, "ucbt");
                              InB      : constant String := Next_Reg (Ctx.E, "ucin");
                              SB_P     : constant String := Next_Reg (Ctx.E, "ucsp");
                              SB_V     : constant String := Next_Reg (Ctx.E, "ucsv");
                              S64      : constant String := Next_Reg (Ctx.E, "ucs6");
                              Sh       : constant String := Next_Reg (Ctx.E, "ucsh");
                              B1       : constant String := Next_Reg (Ctx.E, "ucb1");
                              B8       : constant String := Next_Reg (Ctx.E, "ucb8");
                              BV       : constant String := Next_Reg (Ctx.E, "ucbv");
                              DP       : constant String := Next_Reg (Ctx.E, "ucdp");
                              Nx       : constant String := Next_Reg (Ctx.E, "ucnx");
                              SSz      : constant Natural := Natural'Max (1, STI.Byte_Size);
                           begin
                              Emit_Index_Bounds (Ctx, RTI.Dims (1).Idx_Type, F_R, L_R);
                              NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_R), To_String (L_R)));
                              Emit_Inst (Ctx.E, Nz & " = cmp_gt_s.i64 " & To_String (NB64) & ", 0");
                              Emit_Inst (Ctx.E, AlB & " = select.i64 " & Nz & ", " & To_String (NB64) & ", 1");
                              declare
                                 Buf_U : Unbounded_String;
                              begin
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_ss_alloc", AlB & ", 1", "ptr", Buf_U);
                                 Emit_Inst (Ctx.E, Buf & " = ptr_add.i64 " & To_String (Buf_U) & ", 0");
                              end;
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & To_String (NB64));
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, ByI & " = shr.i64 " & IR & ", 3");
                              Emit_Inst (Ctx.E, BiI & " = and.i64 " & IR & ", 7");
                              Emit_Inst (Ctx.E, InB & " = cmp_lt_s.i64 " & ByI & ", " & Img (SSz));
                              declare
                                 SafeI : constant String := Next_Reg (Ctx.E, "ucsi");
                              begin
                                 Emit_Inst (Ctx.E, SafeI & " = select.i64 " & InB & ", " & ByI & ", 0");
                                 Emit_Inst (Ctx.E, SB_P & " = elem_ptr i8, " & To_String (Src.Reg) & ", " & SafeI);
                              end;
                              Emit_Inst (Ctx.E, SB_V & " = load.i8 " & SB_P & ", align 1");
                              Emit_Inst (Ctx.E, S64 & " = zext.i64 " & SB_V);
                              Emit_Inst (Ctx.E, Sh & " = shr.i64 " & S64 & ", " & BiI);
                              Emit_Inst (Ctx.E, B1 & " = and.i64 " & Sh & ", 1");
                              Emit_Inst (Ctx.E, B8 & " = trunc.i8 " & B1);
                              Emit_Inst (Ctx.E, BV & " = select.i8 " & InB & ", " & B8 & ", 0");
                              Emit_Inst (Ctx.E, DP & " = elem_ptr i8, " & Buf & ", " & IR);
                              Emit_Inst (Ctx.E, "store.i8 " & BV & ", " & DP & ", align 1");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              Res.Kind := EK_Slice; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := To_Unbounded_String ("ptr"); Res.Ada_Type := Res_Type;
                              Res.Align := 1; Res.NDims := 1;
                              Res.First_Regs (1) := F_R; Res.Last_Regs (1) := L_R;
                           end;
                        elsif Is_Bool_Bit_Array (STI) and then RTI.Kind /= TK_Scalar then
                           -- Bool bit-array -> Composite (cd4061a): pack 0/1 BOOLEANs bit-by-bit into target bytes
                           declare
                              SF, SL : Unbounded_String;
                              NB64   : Unbounded_String;
                              DSz    : constant Natural := Natural'Max (1, RTI.Byte_Size);
                              DAl    : constant Natural := Natural'Max (1, RTI.Align);
                              MaxB   : constant Natural := DSz * 8;
                              LtM    : constant String := Next_Reg (Ctx.E, "uplm");
                              Lim    : constant String := Next_Reg (Ctx.E, "upli");
                              Buf    : constant String := Next_Reg (Ctx.E, "upbf");
                              L_H    : constant String := Next_Label (Ctx.E, "upb_h");
                              L_B    : constant String := Next_Label (Ctx.E, "upb_b");
                              L_E    : constant String := Next_Label (Ctx.E, "upb_e");
                              IR     : constant String := Next_Reg (Ctx.E, "upbi");
                              Cnd    : constant String := Next_Reg (Ctx.E, "upbc");
                              SP     : constant String := Next_Reg (Ctx.E, "upsp");
                              SV     : constant String := Next_Reg (Ctx.E, "upsv");
                              B1     : constant String := Next_Reg (Ctx.E, "upb1");
                              ByI    : constant String := Next_Reg (Ctx.E, "upby");
                              BiI    : constant String := Next_Reg (Ctx.E, "upbt");
                              Bi8    : constant String := Next_Reg (Ctx.E, "up8");
                              Sh     : constant String := Next_Reg (Ctx.E, "upsh");
                              DP     : constant String := Next_Reg (Ctx.E, "updp");
                              Cur    : constant String := Next_Reg (Ctx.E, "upcr");
                              Nw     : constant String := Next_Reg (Ctx.E, "upnw");
                              Nx     : constant String := Next_Reg (Ctx.E, "upnx");
                           begin
                              Get_Array_Dim_Bounds (Ctx, Src, 1, SF, SL);
                              NB64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (SF), To_String (SL)));
                              Emit_Inst (Ctx.E, LtM & " = cmp_lt_s.i64 " & To_String (NB64) & ", " & Img (MaxB));
                              Emit_Inst (Ctx.E, Lim & " = select.i64 " & LtM & ", " & To_String (NB64) & ", " & Img (MaxB));
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (DSz) & " x i8], align " & Img (DAl));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (DSz) & ", align " & Img (DAl));
                              Emit_Term (Ctx.E, "jmp " & L_H & "(0)");
                              Emit_Label (Ctx.E, L_H, IR & ": i64");
                              Emit_Inst (Ctx.E, Cnd & " = cmp_lt_s.i64 " & IR & ", " & Lim);
                              Emit_Term (Ctx.E, "br " & Cnd & ", " & L_B & ", " & L_E);
                              Emit_Label (Ctx.E, L_B);
                              Emit_Inst (Ctx.E, SP & " = elem_ptr i8, " & To_String (Src.Reg) & ", " & IR);
                              Emit_Inst (Ctx.E, SV & " = load.i8 " & SP & ", align 1");
                              Emit_Inst (Ctx.E, B1 & " = and.i8 " & SV & ", 1");
                              Emit_Inst (Ctx.E, ByI & " = shr.i64 " & IR & ", 3");
                              Emit_Inst (Ctx.E, BiI & " = and.i64 " & IR & ", 7");
                              Emit_Inst (Ctx.E, Bi8 & " = trunc.i8 " & BiI);
                              Emit_Inst (Ctx.E, Sh & " = shl.i8 " & B1 & ", " & Bi8);
                              Emit_Inst (Ctx.E, DP & " = elem_ptr i8, " & Buf & ", " & ByI);
                              Emit_Inst (Ctx.E, Cur & " = load.i8 " & DP & ", align 1");
                              Emit_Inst (Ctx.E, Nw & " = or.i8 " & Cur & ", " & Sh);
                              Emit_Inst (Ctx.E, "store.i8 " & Nw & ", " & DP & ", align 1");
                              Emit_Inst (Ctx.E, Nx & " = add.i64 " & IR & ", 1 [unsafe_s]");
                              Emit_Term (Ctx.E, "jmp " & L_H & "(" & Nx & ")");
                              Emit_Label (Ctx.E, L_E);
                              -- Sign-extend narrow signed integer single-component record (cd4061a: 5-bit -7 -> i8 -7)
                              if RTI.Kind = TK_Record and then DSz <= 8 then
                                 declare
                                    BR : constant TREE := Subst_Base_Type (Ctx, Res_Type);
                                    CL : constant TREE := (if not Is_Void_Tree (BR) and then KIND (BR) = DN_RECORD then D (SM_COMP_LIST, BR) else CONST_VOID);
                                 begin
                                    if not Is_Void_Tree (CL) and then not Is_Void_Tree (D (AS_DECL_S, CL)) then
                                       declare
                                          DL : SEQ_TYPE := LIST (D (AS_DECL_S, CL));
                                          C1 : TREE;
                                       begin
                                          if not IS_EMPTY (DL) then
                                             POP (DL, C1);
                                             if IS_EMPTY (DL) and then KIND (C1) = DN_VARIABLE_DECL then
                                                declare
                                                   NS : SEQ_TYPE := LIST (D (AS_SOURCE_NAME_S, C1));
                                                   CID : TREE;
                                                begin
                                                   if not IS_EMPTY (NS) then
                                                      POP (NS, CID);
                                                      if IS_EMPTY (NS) then
                                                         declare
                                                            CT  : constant TREE := Subst_Resolve_Type (Ctx, D (SM_OBJ_TYPE, CID));
                                                            CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                                            St_C : Boolean := False;
                                                            CLo, CHi : Integer_64 := 0;
                                                         begin
                                                            if Is_Integer_Type (CT) then
                                                               Get_Discrete_Bounds (CT, St_C, CLo, CHi);
                                                               if not St_C or else CLo < 0 then
                                                                  declare
                                                                     FBits  : constant Natural := CTI.Byte_Size * 8;
                                                                     Lv0    : constant String := Next_Reg (Ctx.E, "rslv");
                                                                     Lv64   : Unbounded_String;
                                                                     Gt0    : constant String := Next_Reg (Ctx.E, "rsg0");
                                                                     LtF    : constant String := Next_Reg (Ctx.E, "rslf");
                                                                     NeedS  : constant String := Next_Reg (Ctx.E, "rsns");
                                                                     ShA    : constant String := Next_Reg (Ctx.E, "rssa");
                                                                     SafeA  : constant String := Next_Reg (Ctx.E, "rsza");
                                                                     ShL    : constant String := Next_Reg (Ctx.E, "rssl");
                                                                     ShR    : constant String := Next_Reg (Ctx.E, "rssr");
                                                                     S64    : constant String := Next_Reg (Ctx.E, "rss6");
                                                                     OutV   : Unbounded_String;
                                                                  begin
                                                                     Emit_Inst (Ctx.E, Lv0 & " = load." & To_String (CTI.Pir_Type) & " " & Buf & ", align " & Img (DAl));
                                                                     Lv64 := To_Unbounded_String (Emit_Int_Convert (Ctx, Lv0, To_String (CTI.Pir_Type), "i64"));
                                                                     Emit_Inst (Ctx.E, Gt0 & " = cmp_gt_s.i64 " & Lim & ", 0");
                                                                     Emit_Inst (Ctx.E, LtF & " = cmp_lt_s.i64 " & Lim & ", " & Img (FBits));
                                                                     Emit_Inst (Ctx.E, NeedS & " = and.i8 " & Gt0 & ", " & LtF);
                                                                     Emit_Inst (Ctx.E, ShA & " = sub.i64 64, " & Lim);
                                                                     Emit_Inst (Ctx.E, SafeA & " = select.i64 " & NeedS & ", " & ShA & ", 0");
                                                                     Emit_Inst (Ctx.E, ShL & " = shl.i64 " & To_String (Lv64) & ", " & SafeA);
                                                                     Emit_Inst (Ctx.E, ShR & " = sar.i64 " & ShL & ", " & SafeA);
                                                                     Emit_Inst (Ctx.E, S64 & " = select.i64 " & NeedS & ", " & ShR & ", " & To_String (Lv64));
                                                                     OutV := To_Unbounded_String (Emit_Int_Convert (Ctx, S64, "i64", To_String (CTI.Pir_Type)));
                                                                     Emit_Inst (Ctx.E, "store." & To_String (CTI.Pir_Type) & " " & To_String (OutV) & ", " & Buf & ", align " & Img (DAl));
                                                                  end;
                                                               end if;
                                                            end if;
                                                         end;
                                                      end if;
                                                   end if;
                                                end;
                                             end if;
                                          end if;
                                       end;
                                    end if;
                                 end;
                              end if;
                              Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := DAl;
                           end;
                        elsif RTI.Kind /= TK_Scalar and then STI.Kind /= TK_Scalar then
                           -- Composite -> Composite: allocate target buffer and copy min(src_sz, dst_sz) bytes
                           declare
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              Sz  : constant Natural := Natural'Max (1, Natural'Min (STI.Byte_Size, RTI.Byte_Size));
                              DSz : constant Natural := Natural'Max (1, RTI.Byte_Size);
                              Al  : constant Natural := Natural'Max (1, RTI.Align);
                           begin
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (DSz) & " x i8], align " & Img (Al));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (DSz) & ", align " & Img (Al));
                              Emit_Inst (Ctx.E, "memcpy " & Buf & ", " & To_String (Src.Reg) & ", " & Img (Sz) & ", align 1");
                              Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Al;
                           end;
                        elsif RTI.Kind /= TK_Scalar and then STI.Kind = TK_Scalar then
                           -- Scalar -> Composite: map enum SM_REP if present (P3-6.2), then store into target buffer
                           declare
                              Src_BT   : constant TREE := Find_Enum_Rep_Type (Ctx, Src.Ada_Type);
                              Store_R  : Unbounded_String := Src.Reg;
                              Store_PT : Unbounded_String := Src.Pir_Type;
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              DSz : constant Natural := Natural'Max (STI.Byte_Size, RTI.Byte_Size);
                              Al  : constant Natural := Natural'Max (STI.Align, RTI.Align);
                           begin
                              if not Is_Void_Tree (Src_BT) then
                                 declare
                                    Rep64 : constant String := Emit_Enum_Pos_To_Rep_I64 (Ctx, Src_BT, To_String (Src.Reg), To_String (Src.Pir_Type));
                                 begin
                                    Store_R := To_Unbounded_String (Emit_Int_Convert (Ctx, Rep64, "i64", To_String (Src.Pir_Type)));
                                 end;
                              end if;
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, DSz)) & " x i8], align " & Img (Natural'Max (1, Al)));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (Natural'Max (1, DSz)) & ", align 1");
                              Emit_Inst (Ctx.E, "store." & To_String (Store_PT) & " " & To_String (Store_R) & ", " & Buf & ", align " & Img (Natural'Max (1, STI.Align)));
                              Res.Kind := EK_Addr; Res.Reg := To_Unbounded_String (Buf);
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Natural'Max (1, RTI.Align);
                           end;
                        elsif RTI.Kind = TK_Scalar and then STI.Kind /= TK_Scalar then
                           -- Composite -> Scalar: copy bytes into aligned temp buffer, load scalar, map enum SM_REP if present (P3-6.2)
                           declare
                              Res_BT : constant TREE := Find_Enum_Rep_Type (Ctx, Res_Type);
                              Buf : constant String := Next_Reg (Ctx.E, "ucb");
                              Sz  : constant Natural := Natural'Max (1, Natural'Min (STI.Byte_Size, RTI.Byte_Size));
                              RSz : constant Natural := Natural'Max (1, RTI.Byte_Size);
                              Al  : constant Natural := Natural'Max (1, RTI.Align);
                              Out_R : Unbounded_String;
                           begin
                              Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (RSz) & " x i8], align " & Img (Al));
                              Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (RSz) & ", align " & Img (Al));
                              Emit_Inst (Ctx.E, "memcpy " & Buf & ", " & To_String (Src.Reg) & ", " & Img (Sz) & ", align 1");
                              Emit_Inst (Ctx.E, R & " = load." & To_String (RTI.Pir_Type) & " " & Buf & ", align " & Img (Al));
                              if not Is_Void_Tree (Res_BT) then
                                 declare
                                    R64  : constant String := Emit_Int_Convert (Ctx, R, To_String (RTI.Pir_Type), "i64",
                                      Is_Unsigned => not Enum_Rep_Has_Negative (Res_BT));
                                    PosR : constant String := Emit_Enum_Rep_I64_To_Pos (Ctx, Res_BT, R64);
                                 begin
                                    Out_R := To_Unbounded_String (Emit_Int_Convert (Ctx, PosR, "i64", To_String (RTI.Pir_Type)));
                                 end;
                              else
                                 Out_R := To_Unbounded_String (R);
                              end if;
                              Res.Kind := EK_Scalar; Res.Reg := Out_R;
                              Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Al;
                           end;
                        elsif To_String (Src.Pir_Type) = To_String (RTI.Pir_Type)
                          or else (STI.Kind = TK_Scalar and then RTI.Kind = TK_Scalar)
                        then
                           -- LRM 13.3 + 13.9: map enumeration SM_REP codes via canonical helpers
                           declare
                              Src_BT      : constant TREE := Find_Enum_Rep_Type (Ctx, Src.Ada_Type);
                              Res_BT      : constant TREE := Find_Enum_Rep_Type (Ctx, Res_Type);
                              Eff_Src     : Expr_Val := Src;
                              Eff_STI     : Type_Info := STI;
                              Src_Has_Rep : constant Boolean := not Is_Void_Tree (Src_BT);
                              Res_Has_Rep : constant Boolean := not Is_Void_Tree (Res_BT);
                           begin
                              if Src_Has_Rep then
                                 declare
                                    RV : constant String :=
                                      Emit_Enum_Pos_To_Rep_I64 (Ctx, Src_BT, To_String (Src.Reg), To_String (Src.Pir_Type));
                                 begin
                                    Eff_Src.Reg       := To_Unbounded_String (RV);
                                    Eff_Src.Pir_Type  := To_Unbounded_String ("i64");
                                    Eff_STI.Pir_Type  := To_Unbounded_String ("i64");
                                    Eff_STI.Byte_Size := 8;
                                    Eff_STI.Align     := 8;
                                 end;
                              end if;

                              if Res_Has_Rep then
                                 declare
                                    SPT  : constant String := To_String (Eff_Src.Pir_Type);
                                    V64  : Unbounded_String;
                                    PosR : Unbounded_String;
                                    Fin  : Unbounded_String;
                                 begin
                                    if SPT = "ptr" then
                                       declare
                                          P2I : constant String := Next_Reg (Ctx.E, "ep2i");
                                       begin
                                          Emit_Inst (Ctx.E, P2I & " = ptrtoint.i64 " & To_String (Eff_Src.Reg));
                                          V64 := To_Unbounded_String (P2I);
                                       end;
                                    elsif SPT = "f32" then
                                       declare
                                          B32 : constant String := Next_Reg (Ctx.E, "ef32");
                                       begin
                                          Emit_Inst (Ctx.E, B32 & " = bitcast.i32 " & To_String (Eff_Src.Reg));
                                          V64 := To_Unbounded_String (Emit_Int_Convert (Ctx, B32, "i32", "i64", Is_Unsigned => True));
                                       end;
                                    elsif SPT = "f64" then
                                       declare
                                          B64 : constant String := Next_Reg (Ctx.E, "ef64");
                                       begin
                                          Emit_Inst (Ctx.E, B64 & " = bitcast.i64 " & To_String (Eff_Src.Reg));
                                          V64 := To_Unbounded_String (B64);
                                       end;
                                    else
                                       V64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (Eff_Src.Reg), SPT, "i64",
                                         Is_Unsigned => Is_Unsigned_Scalar (Eff_Src.Ada_Type, SPT)));
                                    end if;
                                    PosR := To_Unbounded_String (Emit_Enum_Rep_I64_To_Pos (Ctx, Res_BT, To_String (V64)));
                                    Fin  := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (PosR), "i64", To_String (RTI.Pir_Type)));
                                    Res.Kind := EK_Scalar; Res.Reg := Fin;
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                                 end;
                              elsif Src_Has_Rep and then To_String (RTI.Pir_Type) /= "ptr"
                                and then To_String (RTI.Pir_Type) /= "f32" and then To_String (RTI.Pir_Type) /= "f64"
                              then
                                 declare
                                    Fin : constant String := Emit_Int_Convert (Ctx, To_String (Eff_Src.Reg), "i64", To_String (RTI.Pir_Type));
                                 begin
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (Fin);
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := RTI.Align;
                                 end;
                              elsif To_String (Eff_Src.Pir_Type) = To_String (RTI.Pir_Type) then
                                 Res := Eff_Src;
                                 Res.Ada_Type := Res_Type;
                              elsif To_String (Eff_Src.Pir_Type) = "ptr" and then To_String (RTI.Pir_Type) = "i64" then
                                 Emit_Inst (Ctx.E, R & " = ptrtoint.i64 " & To_String (Eff_Src.Reg));
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                              elsif To_String (RTI.Pir_Type) = "ptr" and then To_String (Eff_Src.Pir_Type) = "i64" then
                                 Emit_Inst (Ctx.E, R & " = inttoptr.ptr " & To_String (Eff_Src.Reg));
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                              elsif Eff_STI.Byte_Size /= RTI.Byte_Size
                                or else To_String (Eff_Src.Pir_Type) = "ptr"
                                or else To_String (RTI.Pir_Type) = "ptr"
                              then
                                 declare
                                    Buf : constant String := Next_Reg (Ctx.E, "ucb");
                                    Sz  : constant Natural := Natural'Max (Eff_STI.Byte_Size, RTI.Byte_Size);
                                    Al  : constant Natural := Natural'Max (Eff_STI.Align, RTI.Align);
                                 begin
                                    Emit_Entry_Alloca (Ctx.E, Buf & " = alloca [" & Img (Natural'Max (1, Sz)) & " x i8], align " & Img (Natural'Max (1, Al)));
                                    Emit_Inst (Ctx.E, "memset " & Buf & ", 0, " & Img (Natural'Max (1, Sz)) & ", align 1");
                                    Emit_Inst (Ctx.E, "store." & To_String (Eff_Src.Pir_Type) & " " & To_String (Eff_Src.Reg) & ", " & Buf & ", align " & Img (Natural'Max (1, Eff_STI.Align)));
                                    Emit_Inst (Ctx.E, R & " = load." & To_String (RTI.Pir_Type) & " " & Buf & ", align " & Img (Natural'Max (1, RTI.Align)));
                                    Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                    Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type; Res.Align := Natural'Max (1, RTI.Align);
                                 end;
                              else
                                 Emit_Inst (Ctx.E, R & " = bitcast." & To_String (RTI.Pir_Type) & " " & To_String (Eff_Src.Reg));
                                 Res.Kind := EK_Scalar; Res.Reg := To_Unbounded_String (R);
                                 Res.Pir_Type := RTI.Pir_Type; Res.Ada_Type := Res_Type;
                              end if;
                           end;
                        end if;
                        return Res;
                     end;
                  end if;
               end;
            end if;
         end;
      end if;

      -- Helper: extract (ptr, first, len) from an Ada String Expr_Val
      declare
      procedure Emit_Str_Ptr_Len
        (SV    : Expr_Val;
         Ptr_S : out Unbounded_String;
         F_S   : out Unbounded_String;
         Len_S : out Unbounded_String)
      is
         F, L : Unbounded_String;
      begin
         Get_Array_Dim_Bounds (Ctx, SV, 1, F, L);
         declare
            L64 : constant String := Emit_Dim_Len_I64 (Ctx, To_String (F), To_String (L));
            F32 : constant String := Emit_Int_Convert (Ctx, To_String (F), "i64", "i32");
            Cl  : constant String := Emit_Int_Convert (Ctx, L64, "i64", "i32");
         begin
            Ptr_S := SV.Reg;
            F_S   := To_Unbounded_String (F32);
            Len_S := To_Unbounded_String (Cl);
         end;
      end Emit_Str_Ptr_Len;

      function Is_File_Arg (Arg_Node : TREE) return Boolean is
         ATy : constant TREE := (if not Is_Void_Tree (Arg_Node) and then KIND (Arg_Node) in CLASS_EXP
                                 then Subst_Tree (Ctx, D (SM_EXP_TYPE, Arg_Node)) else CONST_VOID);
      begin
         return not Is_Void_Tree (ATy) and then Is_Record_Type (ATy);
      end Is_File_Arg;

      function Is_Str_Arg (Arg_Node : TREE) return Boolean is
         ATy : constant TREE := (if not Is_Void_Tree (Arg_Node) and then KIND (Arg_Node) in CLASS_EXP
                                 then Subst_Tree (Ctx, D (SM_EXP_TYPE, Arg_Node)) else CONST_VOID);
      begin
         if not Is_Void_Tree (Arg_Node) and then KIND (Arg_Node) = DN_STRING_LITERAL then
            return True;
         end if;
         return not Is_Void_Tree (ATy) and then Is_String_Type (ATy);
      end Is_Str_Arg;

      function Emit_File_Ptr_Or_Null (Has_File : Boolean) return String is
      begin
         if Has_File then
            declare
               FV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
            begin
               return To_String (FV.Reg);
            end;
         end if;
         return "0";
      end Emit_File_Ptr_Or_Null;

      -- P3-6.11 & P3-6.17: Shared DATA_ERROR range check for TEXT_IO.*_IO, SEQUENTIAL_IO, and DIRECT_IO
      procedure Emit_Raise_Data_If_RTL (Cond_R : String) is
         L_Fail : constant String := Next_Label (Ctx.E, "iode");
         L_Ok   : constant String := Next_Label (Ctx.E, "iook");
         Dummy  : Unbounded_String;
      begin
         Emit_Term (Ctx.E, "br " & Cond_R & ", " & L_Fail & ", " & L_Ok);
         Emit_Label (Ctx.E, L_Fail);
         Emit_Call_Or_Invoke (Ctx, "@__ada_raise_data_error", "", "", Dummy);
         Emit_Term (Ctx.E, "unreachable");
         Emit_Label (Ctx.E, L_Ok);
      end Emit_Raise_Data_If_RTL;

      procedure Emit_Inst_Range_Data_Check_RTL (Val_R : String; Pir_T : String; Check_T : TREE) is
         RT : constant TREE := Resolve_Type_Spec (Check_T);
      begin
         if Is_Void_Tree (RT) or else not Ctx.Enable_Checks then
            return;
         end if;
         if Is_Discrete_Type (RT) then
            declare
               St : Boolean;
               Lo, Hi : Integer_64;
               Is_U  : constant Boolean := Is_Unsigned_Scalar (RT, Pir_T);
               Lt_Op : constant String := (if Is_U then "cmp_lt_u." else "cmp_lt_s.");
               Gt_Op : constant String := (if Is_U then "cmp_gt_u." else "cmp_gt_s.");
            begin
               Get_Discrete_Bounds (RT, St, Lo, Hi);
               if St then
                  declare
                     LoC : constant String := Next_Reg (Ctx.E, "id1");
                     HiC : constant String := Next_Reg (Ctx.E, "id2");
                     Bad : constant String := Next_Reg (Ctx.E, "idb");
                  begin
                     Emit_Inst (Ctx.E, LoC & " = " & Lt_Op & Pir_T & " " & Val_R & ", " & Img (Long_Long_Integer (Lo)));
                     Emit_Inst (Ctx.E, HiC & " = " & Gt_Op & Pir_T & " " & Val_R & ", " & Img (Long_Long_Integer (Hi)));
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                     Emit_Raise_Data_If_RTL (Bad);
                  end;
               elsif Ctx.Saved_Type_Ranges.Contains (Tree_Key (RT))
                 and then (Ctx.Saved_Type_Ranges.Element (Tree_Key (RT)).Is_Global
                           or else Ctx.Saved_Type_Ranges.Element (Tree_Key (RT)).Owner_Subp = Ctx.Cur_Subp_Key)
               then
                  declare
                     SR  : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (RT));
                     LR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                     HR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                     SU  : constant Boolean := Is_Unsigned_Scalar (RT, To_String (SR.Pir_Type));
                     L_R : constant String := Emit_Int_Convert (Ctx, LR0, To_String (SR.Pir_Type), Pir_T, Is_Unsigned => SU);
                     H_R : constant String := Emit_Int_Convert (Ctx, HR0, To_String (SR.Pir_Type), Pir_T, Is_Unsigned => SU);
                     LoC : constant String := Next_Reg (Ctx.E, "id1");
                     HiC : constant String := Next_Reg (Ctx.E, "id2");
                     Bad : constant String := Next_Reg (Ctx.E, "idb");
                  begin
                     Emit_Inst (Ctx.E, LoC & " = " & Lt_Op & Pir_T & " " & Val_R & ", " & L_R);
                     Emit_Inst (Ctx.E, HiC & " = " & Gt_Op & Pir_T & " " & Val_R & ", " & H_R);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                     Emit_Raise_Data_If_RTL (Bad);
                  end;
               elsif KIND (RT) in DN_INTEGER | DN_ENUMERATION
                 and then not Is_Void_Tree (D (SM_RANGE, RT))
                 and then KIND (D (SM_RANGE, RT)) = DN_RANGE
               then
                  declare
                     Rng : constant TREE := D (SM_RANGE, RT);
                     E1  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP1, Rng), RT);
                     E2  : constant Expr_Val := Emit_Expr (Ctx, D (AS_EXP2, Rng), RT);
                     U1  : constant Boolean := Is_Unsigned_Scalar (RT, To_String (E1.Pir_Type));
                     U2  : constant Boolean := Is_Unsigned_Scalar (RT, To_String (E2.Pir_Type));
                     L_R : constant String := Emit_Int_Convert (Ctx, To_String (E1.Reg), To_String (E1.Pir_Type), Pir_T, Is_Unsigned => U1);
                     H_R : constant String := Emit_Int_Convert (Ctx, To_String (E2.Reg), To_String (E2.Pir_Type), Pir_T, Is_Unsigned => U2);
                     LoC : constant String := Next_Reg (Ctx.E, "id1");
                     HiC : constant String := Next_Reg (Ctx.E, "id2");
                     Bad : constant String := Next_Reg (Ctx.E, "idb");
                  begin
                     Emit_Inst (Ctx.E, LoC & " = " & Lt_Op & Pir_T & " " & Val_R & ", " & L_R);
                     Emit_Inst (Ctx.E, HiC & " = " & Gt_Op & Pir_T & " " & Val_R & ", " & H_R);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                     Emit_Raise_Data_If_RTL (Bad);
                  end;
               end if;
            end;
         elsif Is_Float_Type (RT) or else Is_Fixed_Type (RT) then
            declare
               V64 : constant String :=
                 (if Pir_T = "f64" then Val_R
                  elsif Pir_T = "f32" then Emit_Scalar_Coerce (Ctx, Val_R, "f32", "f64")
                  else Val_R);
               Is_F64_Cmp : constant Boolean := Pir_T = "f64" or else Pir_T = "f32";
               Rng : constant TREE := D (SM_RANGE, RT);
            begin
               -- P3-6.17: Consult Saved_Type_Ranges for dynamic float/fixed subtypes!
               if Ctx.Saved_Type_Ranges.Contains (Tree_Key (RT))
                 and then (Ctx.Saved_Type_Ranges.Element (Tree_Key (RT)).Is_Global
                           or else Ctx.Saved_Type_Ranges.Element (Tree_Key (RT)).Owner_Subp = Ctx.Cur_Subp_Key)
               then
                  declare
                     SR  : constant Saved_Range_Info := Ctx.Saved_Type_Ranges.Element (Tree_Key (RT));
                     LR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Lo_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                     HR0 : constant String := Load_Saved_Slot (Ctx, To_String (SR.Hi_Ptr), To_String (SR.Pir_Type), SR.Align, SR.Is_Global);
                     LoC : constant String := Next_Reg (Ctx.E, "fd1");
                     HiC : constant String := Next_Reg (Ctx.E, "fd2");
                     Bad : constant String := Next_Reg (Ctx.E, "fdb");
                  begin
                     if Is_F64_Cmp then
                        declare
                           L_R, H_R : Unbounded_String;
                        begin
                           if Is_Fixed_Type (RT) then
                              declare
                                 F1 : constant String := Next_Reg (Ctx.E, "fxl1");
                                 S1 : constant String := Next_Reg (Ctx.E, "fxs1");
                                 F2 : constant String := Next_Reg (Ctx.E, "fxl2");
                                 S2 : constant String := Next_Reg (Ctx.E, "fxs2");
                              begin
                                 Emit_Inst (Ctx.E, F1 & " = sitofp.f64 " & LR0);
                                 Emit_Inst (Ctx.E, S1 & " = fmul.f64 " & F1 & ", " & Format_Float64 (Fixed_Small (RT)));
                                 Emit_Inst (Ctx.E, F2 & " = sitofp.f64 " & HR0);
                                 Emit_Inst (Ctx.E, S2 & " = fmul.f64 " & F2 & ", " & Format_Float64 (Fixed_Small (RT)));
                                 L_R := To_Unbounded_String (S1);
                                 H_R := To_Unbounded_String (S2);
                              end;
                           else
                              L_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, LR0, To_String (SR.Pir_Type), "f64"));
                              H_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, HR0, To_String (SR.Pir_Type), "f64"));
                           end if;
                           Emit_Inst (Ctx.E, LoC & " = fcmp_ult.f64 " & V64 & ", " & To_String (L_R));
                           Emit_Inst (Ctx.E, HiC & " = fcmp_ugt.f64 " & V64 & ", " & To_String (H_R));
                        end;
                     else
                        -- Raw fixed-point integer ticks comparison (SEQUENTIAL_IO/DIRECT_IO)
                        declare
                           L_R : constant String := Emit_Int_Convert (Ctx, LR0, To_String (SR.Pir_Type), Pir_T);
                           H_R : constant String := Emit_Int_Convert (Ctx, HR0, To_String (SR.Pir_Type), Pir_T);
                        begin
                           Emit_Inst (Ctx.E, LoC & " = cmp_lt_s." & Pir_T & " " & Val_R & ", " & L_R);
                           Emit_Inst (Ctx.E, HiC & " = cmp_gt_s." & Pir_T & " " & Val_R & ", " & H_R);
                        end;
                     end if;
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                     Emit_Raise_Data_If_RTL (Bad);
                  end;
               elsif not Is_Void_Tree (Rng) and then KIND (Rng) = DN_RANGE then
                  declare
                     E1 : constant TREE := D (AS_EXP1, Rng);
                     E2 : constant TREE := D (AS_EXP2, Rng);
                     LoF, HiF : IEEE_Float_64;
                     Have_Lo, Have_Hi : Boolean := False;
                  begin
                     if Has_Static_Val (Safe_SM_Value (E1)) then
                        LoF := Eval_Float64 (Safe_SM_Value (E1));
                        Have_Lo := True;
                     end if;
                     if Has_Static_Val (Safe_SM_Value (E2)) then
                        HiF := Eval_Float64 (Safe_SM_Value (E2));
                        Have_Hi := True;
                     end if;
                     if Is_F64_Cmp then
                        if Have_Lo and then Have_Hi then
                           declare
                              LoC : constant String := Next_Reg (Ctx.E, "fd1");
                              HiC : constant String := Next_Reg (Ctx.E, "fd2");
                              Bad : constant String := Next_Reg (Ctx.E, "fdb");
                           begin
                              Emit_Inst (Ctx.E, LoC & " = fcmp_ult.f64 " & V64 & ", " & Format_Float64 (LoF));
                              Emit_Inst (Ctx.E, HiC & " = fcmp_ugt.f64 " & V64 & ", " & Format_Float64 (HiF));
                              Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                              Emit_Raise_Data_If_RTL (Bad);
                           end;
                        else
                           declare
                              EV1 : constant Expr_Val := Emit_Expr (Ctx, E1, RT);
                              EV2 : constant Expr_Val := Emit_Expr (Ctx, E2, RT);
                              L_R, H_R : Unbounded_String;
                              LoC : constant String := Next_Reg (Ctx.E, "fd1");
                              HiC : constant String := Next_Reg (Ctx.E, "fd2");
                              Bad : constant String := Next_Reg (Ctx.E, "fdb");
                           begin
                              if Is_Fixed_Type (RT) then
                                 declare
                                    F1 : constant String := Next_Reg (Ctx.E, "fxl1");
                                    S1 : constant String := Next_Reg (Ctx.E, "fxs1");
                                    F2 : constant String := Next_Reg (Ctx.E, "fxl2");
                                    S2 : constant String := Next_Reg (Ctx.E, "fxs2");
                                 begin
                                    Emit_Inst (Ctx.E, F1 & " = sitofp.f64 " & To_String (EV1.Reg));
                                    Emit_Inst (Ctx.E, S1 & " = fmul.f64 " & F1 & ", " & Format_Float64 (Fixed_Small (RT)));
                                    Emit_Inst (Ctx.E, F2 & " = sitofp.f64 " & To_String (EV2.Reg));
                                    Emit_Inst (Ctx.E, S2 & " = fmul.f64 " & F2 & ", " & Format_Float64 (Fixed_Small (RT)));
                                    L_R := To_Unbounded_String (S1);
                                    H_R := To_Unbounded_String (S2);
                                 end;
                              else
                                 L_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), "f64"));
                                 H_R := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (EV2.Reg), To_String (EV2.Pir_Type), "f64"));
                              end if;
                              Emit_Inst (Ctx.E, LoC & " = fcmp_ult.f64 " & V64 & ", " & To_String (L_R));
                              Emit_Inst (Ctx.E, HiC & " = fcmp_ugt.f64 " & V64 & ", " & To_String (H_R));
                              Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                              Emit_Raise_Data_If_RTL (Bad);
                           end;
                        end if;
                     else
                        -- Raw fixed-point integer ticks comparison (SEQUENTIAL_IO/DIRECT_IO)
                        declare
                           EV1 : constant Expr_Val := Emit_Expr (Ctx, E1, RT);
                           EV2 : constant Expr_Val := Emit_Expr (Ctx, E2, RT);
                           L_R : constant String := Emit_Int_Convert (Ctx, To_String (EV1.Reg), To_String (EV1.Pir_Type), Pir_T);
                           H_R : constant String := Emit_Int_Convert (Ctx, To_String (EV2.Reg), To_String (EV2.Pir_Type), Pir_T);
                           LoC : constant String := Next_Reg (Ctx.E, "fd1");
                           HiC : constant String := Next_Reg (Ctx.E, "fd2");
                           Bad : constant String := Next_Reg (Ctx.E, "fdb");
                        begin
                           Emit_Inst (Ctx.E, LoC & " = cmp_lt_s." & Pir_T & " " & Val_R & ", " & L_R);
                           Emit_Inst (Ctx.E, HiC & " = cmp_gt_s." & Pir_T & " " & Val_R & ", " & H_R);
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                           Emit_Raise_Data_If_RTL (Bad);
                        end;
                     end if;
                  end;
               end if;
            end;
         end if;
      end Emit_Inst_Range_Data_Check_RTL;

      -- P3-6.13: Resolve scalar OUT target (unwrapping DN_CONVERSION chains & detecting record bitfields!)
      type Tree_Arr_8 is array (1 .. 8) of TREE;
      type RTL_Out_Target is record
         Is_BF      : Boolean := False;
         Dst_Ptr    : Unbounded_String;
         BF_FE      : Field_Entry;
         Inner_T    : TREE := CONST_VOID;
         Inner_Pir  : Unbounded_String;
         Inner_Al   : Natural := 4;
         N_Convs    : Natural := 0;
         Conv_Ts    : Tree_Arr_8 := (others => CONST_VOID);
         Outer_T    : TREE := CONST_VOID;
      end record;

      function Resolve_RTL_Out_Target (Act_N : TREE) return RTL_Out_Target is
         Tgt : RTL_Out_Target;
         Cur : TREE := Act_N;
      begin
         while not Is_Void_Tree (Cur) and then KIND (Cur) = DN_PARENTHESIZED loop
            Cur := D (AS_EXP, Cur);
         end loop;
         if not Is_Void_Tree (Cur) and then KIND (Cur) in CLASS_EXP then
            Tgt.Outer_T := Subst_Tree (Ctx, D (SM_EXP_TYPE, Cur));
         end if;
         while not Is_Void_Tree (Cur) and then KIND (Cur) = DN_CONVERSION loop
            if Tgt.N_Convs < 8 then
               Tgt.N_Convs := Tgt.N_Convs + 1;
               Tgt.Conv_Ts (Tgt.N_Convs) := Subst_Tree (Ctx, D (SM_EXP_TYPE, Cur));
            end if;
            Cur := D (AS_EXP, Cur);
            while not Is_Void_Tree (Cur) and then KIND (Cur) = DN_PARENTHESIZED loop
               Cur := D (AS_EXP, Cur);
            end loop;
         end loop;
         -- Check if Cur is a bitfield renaming or selected bitfield component
         if not Is_Void_Tree (Cur) and then KIND (Cur) = DN_USED_OBJECT_ID then
            declare
               OD : constant TREE := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Cur)));
            begin
               if not Is_Void_Tree (OD) and then Ctx.Vars.Contains (Tree_Key (OD))
                 and then Ctx.Vars.Element (Tree_Key (OD)).Is_Bitfield
               then
                  declare
                     VO : constant Var_Info := Ctx.Vars.Element (Tree_Key (OD));
                     TI : constant Type_Info := Get_Type_Info (Ctx, VO.Ada_Type);
                  begin
                     Tgt.Is_BF     := True;
                     Tgt.Dst_Ptr   := To_Unbounded_String (Emit_Var_Addr (Ctx, VO));
                     Tgt.BF_FE     := VO.BF_FE;
                     Tgt.Inner_T   := VO.Ada_Type;
                     Tgt.Inner_Pir := TI.Pir_Type;
                     Tgt.Inner_Al  := Natural'Max (1, TI.Align);
                     return Tgt;
                  end;
               end if;
            end;
         elsif not Is_Void_Tree (Cur) and then KIND (Cur) = DN_SELECTED then
            declare
               Des : TREE := D (AS_DESIGNATOR, Cur);
               Def_F : TREE;
            begin
               while not Is_Void_Tree (Des) and then KIND (Des) = DN_SELECTED loop
                  Des := D (AS_DESIGNATOR, Des);
               end loop;
               Def_F := Canonical_Def (Subst_Tree (Ctx, Get_Name_Defn (Des)));
               if not Is_Void_Tree (Def_F) and then KIND (Def_F) in DN_COMPONENT_ID | DN_DISCRIMINANT_ID then
                  declare
                     Pfx_E  : constant TREE := D (AS_NAME, Cur);
                     Pfx_Ts : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_EXP_TYPE, Pfx_E)));
                     Rec_Ts : constant TREE :=
                       (if Is_Access_Type (Pfx_Ts) then Resolve_Type_Spec (Subst_Tree (Ctx, D (SM_DESIG_TYPE, Pfx_Ts))) else Pfx_Ts);
                     Dummy_RTI : constant Type_Info := Get_Type_Info (Ctx, Rec_Ts);
                     pragma Unreferenced (Dummy_RTI);
                     CK : constant Unsigned_32 := Tree_Key (Def_F);
                     CT : constant TREE := Subst_Tree (Ctx, D (SM_OBJ_TYPE, Def_F));
                     CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                  begin
                     if Ctx.Fields.Contains (CK) and then Is_Bitfield (Ctx.Fields.Element (CK)) and then CTI.Kind = TK_Scalar then
                        declare
                           PLV : constant Expr_Val := Emit_LValue (Ctx, Pfx_E);
                           PT_P : constant TREE := Resolve_Type_Spec (Subst_Tree (Ctx, PLV.Ada_Type));
                           RP : Unbounded_String := PLV.Reg;
                        begin
                           if Is_Access_Type (PT_P) then
                              if PLV.Kind = EK_Addr then
                                 declare
                                    LP : constant String := Next_Reg (Ctx.E, "rbfp");
                                 begin
                                    Emit_Inst (Ctx.E, LP & " = load.ptr " & To_String (RP) & ", align 8");
                                    RP := To_Unbounded_String (LP);
                                 end;
                              end if;
                              Emit_Null_Check (Ctx, To_String (RP));
                           end if;
                           if Ctx.Fields.Element (CK).In_Union then
                              Emit_Variant_Check_For_Field (Ctx, To_String (RP), Rec_Ts, Def_F, CK);
                           end if;
                           Tgt.Is_BF     := True;
                           Tgt.Dst_Ptr   := RP;
                           Tgt.BF_FE     := Ctx.Fields.Element (CK);
                           Tgt.Inner_T   := CT;
                           Tgt.Inner_Pir := CTI.Pir_Type;
                           Tgt.Inner_Al  := 1;
                           return Tgt;
                        end;
                     end if;
                  end;
               end if;
            end;
         end if;
         declare
            LV : constant Expr_Val := Emit_LValue (Ctx, Cur);
            TI : constant Type_Info := Get_Type_Info (Ctx, LV.Ada_Type);
         begin
            Tgt.Dst_Ptr   := LV.Reg;
            Tgt.Inner_T   := LV.Ada_Type;
            Tgt.Inner_Pir := TI.Pir_Type;
            Tgt.Inner_Al  := Natural'Max (1, LV.Align);
            if Is_Void_Tree (Tgt.Outer_T) then
               Tgt.Outer_T := LV.Ada_Type;
            end if;
            return Tgt;
         end;
      end Resolve_RTL_Out_Target;

      procedure Emit_RTL_Out_Store
        (Tgt     : RTL_Out_Target;
         Val_Reg : String;
         Val_Pir : String;
         Val_T   : TREE)
      is
         Cur_R : Unbounded_String := To_Unbounded_String (Val_Reg);
         Cur_P : Unbounded_String := To_Unbounded_String (Val_Pir);
         Cur_T : TREE := Val_T;
      begin
         for I in 1 .. Tgt.N_Convs loop
            declare
               CT  : constant TREE := Tgt.Conv_Ts (I);
               CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
               Next_R : constant String :=
                 Emit_Typed_Scalar_Convert (Ctx, To_String (Cur_R), Cur_T, To_String (Cur_P), CT, To_String (CTI.Pir_Type));
            begin
               Emit_Range_Check (Ctx, Next_R, To_String (CTI.Pir_Type), CT);
               Cur_R := To_Unbounded_String (Next_R);
               Cur_P := CTI.Pir_Type;
               Cur_T := CT;
            end;
         end loop;
         declare
            Fin_R : constant String :=
              (if Tgt.N_Convs > 0 or else (not Is_Void_Tree (Cur_T) and then Cur_T /= Tgt.Inner_T)
               then Emit_Typed_Scalar_Convert (Ctx, To_String (Cur_R), Cur_T, To_String (Cur_P), Tgt.Inner_T, To_String (Tgt.Inner_Pir))
               else Emit_Scalar_Coerce (Ctx, To_String (Cur_R), To_String (Cur_P), To_String (Tgt.Inner_Pir)));
         begin
            if Int_Bits_Of_Pir (To_String (Cur_P)) > Int_Bits_Of_Pir (To_String (Tgt.Inner_Pir))
              and then Is_Discrete_Type (Tgt.Inner_T)
            then
               Emit_Range_Check (Ctx, To_String (Cur_R), To_String (Cur_P), Tgt.Inner_T);
            end if;
            Emit_Range_Check (Ctx, Fin_R, To_String (Tgt.Inner_Pir), Tgt.Inner_T);
            Emit_Discriminant_Check (Ctx, Fin_R, Tgt.Inner_T, Is_Access => True);
            if Tgt.Is_BF then
               Emit_Bitfield_Store (Ctx, To_String (Tgt.Dst_Ptr), Tgt.BF_FE, Fin_R, To_String (Tgt.Inner_Pir), Tgt.Inner_T);
            else
               Emit_Inst (Ctx.E, "store." & To_String (Tgt.Inner_Pir) & " " & Fin_R & ", " & To_String (Tgt.Dst_Ptr) & ", align " & Img (Tgt.Inner_Al));
            end if;
         end;
      end Emit_RTL_Out_Store;

      -- Check if enclosing package is an instantiation of TEXT_IO.{INTEGER,FLOAT,FIXED,ENUMERATION}_IO,
      -- SEQUENTIAL_IO, or DIRECT_IO
      begin
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
                     -- Extract instantiated generic parameter type (first DN_SUBTYPE_DECL in SM_DECL_S)
                     if (GP_N = "INTEGER_IO" or else GP_N = "FLOAT_IO"
                         or else GP_N = "FIXED_IO" or else GP_N = "ENUMERATION_IO")
                       and then not Is_Void_Tree (Gen_P)
                       and then KIND (Gen_P) in CLASS_SOURCE_NAME
                       and then not Is_Void_Tree (D (XD_REGION, Gen_P))
                       and then Sym_Name (D (XD_REGION, Gen_P)) = "TEXT_IO"
                       and then Is_Library_Level (D (XD_REGION, Gen_P))
                     then
                        declare
                           Inst_T : TREE := CONST_VOID;
                           DS     : constant TREE := D (SM_DECL_S, PUD);

                           procedure Emit_Field_Check (R32 : String) is
                           begin
                              if Ctx.Enable_Checks then
                                 declare
                                    Bad : constant String := Next_Reg (Ctx.E, "iofb");
                                 begin
                                    Emit_Inst (Ctx.E, Bad & " = cmp_lt_s.i32 " & R32 & ", 0");
                                    Emit_Check_Fail_If (Ctx, Bad);
                                 end;
                              end if;
                           end Emit_Field_Check;

                           procedure Emit_Base_Check (R32 : String) is
                           begin
                              if Ctx.Enable_Checks then
                                 declare
                                    LoC : constant String := Next_Reg (Ctx.E, "iob1");
                                    HiC : constant String := Next_Reg (Ctx.E, "iob2");
                                    Bad : constant String := Next_Reg (Ctx.E, "iobb");
                                 begin
                                    Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i32 " & R32 & ", 2");
                                    Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i32 " & R32 & ", 16");
                                    Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                                    Emit_Check_Fail_If (Ctx, Bad);
                                 end;
                              end if;
                           end Emit_Base_Check;

                           procedure Emit_Inst_Range_Data_Check (Val_R : String; Pir_T : String; Check_T : TREE) is
                           begin
                              Emit_Inst_Range_Data_Check_RTL (Val_R, Pir_T, Check_T);
                           end Emit_Inst_Range_Data_Check;
                        begin
                           if not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS)) then
                              declare
                                 D1 : constant TREE := HEAD (LIST (DS));
                              begin
                                 if KIND (D1) = DN_SUBTYPE_DECL then
                                    Inst_T := Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (AS_SOURCE_NAME, D1)));
                                 end if;
                              end;
                           end if;

                        if Name = "PUT" then
                           Handled := True;
                           if Is_Str_Arg (Args (1)) then
                              -- PUT(TO : out STRING; ITEM : ...; ...)
                              declare
                                 Dst : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 DP, DF, DL : Unbounded_String;
                                 Dummy : Unbounded_String;
                              begin
                                 Emit_Str_Ptr_Len (Dst, DP, DF, DL);
                                 pragma Unreferenced (DF);
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       IV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                       BV  : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                       I64 : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i64",
                                         Is_Unsigned => Is_Unsigned_Scalar (IV.Ada_Type, To_String (IV.Pir_Type)));
                                       B32 : constant String := Emit_Int_Convert (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, I64, "i64", Inst_T);
                                       Emit_Base_Check (B32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_i64_str",
                                         To_String (DP) & ", " & To_String (DL) & ", " & I64 & ", " & B32, "", Dummy);
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       FV  : constant Expr_Val := Emit_Expr (Ctx, Args (2), Inst_T);
                                       AfV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                       ExV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
                                       F64 : Unbounded_String := FV.Reg;
                                       Af32 : constant String := Emit_Int_Convert (Ctx, To_String (AfV.Reg), To_String (AfV.Pir_Type), "i32");
                                       Ex32 : constant String := Emit_Int_Convert (Ctx, To_String (ExV.Reg), To_String (ExV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (FV.Reg), To_String (FV.Pir_Type), Inst_T);
                                       Emit_Field_Check (Af32);
                                       Emit_Field_Check (Ex32);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Fl : constant String := Next_Reg (Ctx.E, "fxf");
                                             Sc : constant String := Next_Reg (Ctx.E, "fxs");
                                             Sm_T : constant TREE := (if not Is_Void_Tree (Inst_T) then Inst_T else FV.Ada_Type);
                                          begin
                                             Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (FV.Reg));
                                             Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (Sm_T)));
                                             F64 := To_Unbounded_String (Sc);
                                          end;
                                       elsif To_String (FV.Pir_Type) = "f32" then
                                          F64 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (FV.Reg), "f32", "f64"));
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_f64_str",
                                         To_String (DP) & ", " & To_String (DL) & ", " & To_String (F64) & ", " & Af32 & ", " & Ex32, "", Dummy);
                                    end;
                                 else
                                    -- ENUMERATION_IO.PUT(TO, ITEM, SET)
                                    declare
                                       EV   : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                       SetV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                       BT   : constant TREE := Base_Type_Spec (EV.Ada_Type);
                                       PS   : Unbounded_String;
                                       Cnt  : Natural;
                                       Idx64 : constant String := Emit_Int_Convert (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), "i64", Is_Unsigned => True);
                                       PA    : constant String := Next_Reg (Ctx.E, "epa");
                                       PE    : constant String := Next_Reg (Ctx.E, "epe");
                                       SP    : constant String := Next_Reg (Ctx.E, "esp");
                                       LC    : constant String := Next_Reg (Ctx.E, "elc");
                                       LC32  : constant String := Next_Reg (Ctx.E, "elc32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), Inst_T);
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       pragma Unreferenced (Cnt);
                                       Emit_Inst (Ctx.E, PA & " = addr " & To_String (PS));
                                       Emit_Inst (Ctx.E, PE & " = elem_ptr ptr, " & PA & ", " & Idx64);
                                       Emit_Inst (Ctx.E, SP & " = load.ptr " & PE & ", align 8");
                                       Emit_Inst (Ctx.E, LC & " = cmp_eq." & To_String (SetV.Pir_Type) & " " & To_String (SetV.Reg) & ", 0");
                                       Emit_Inst (Ctx.E, LC32 & " = zext.i32 " & LC);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_enum_str",
                                         To_String (DP) & ", " & To_String (DL) & ", " & SP & ", " & LC32, "", Dummy);
                                    end;
                                 end if;
                              end;
                              return Res;
                           else
                              -- PUT([FILE,] ITEM, ...)
                              declare
                                 Has_F : constant Boolean := Is_File_Arg (Args (1));
                                 Off   : constant Natural := (if Has_F then 1 else 0);
                                 FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
                                 Dummy : Unbounded_String;
                              begin
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       IV  : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
                                       WV  : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       BV  : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                                       I64 : constant String := Emit_Int_Convert (Ctx, To_String (IV.Reg), To_String (IV.Pir_Type), "i64",
                                         Is_Unsigned => Is_Unsigned_Scalar (IV.Ada_Type, To_String (IV.Pir_Type)));
                                       W32 : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       B32 : constant String := Emit_Int_Convert (Ctx, To_String (BV.Reg), To_String (BV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, I64, "i64", Inst_T);
                                       Emit_Field_Check (W32);
                                       Emit_Base_Check (B32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_i64",
                                         FP & ", " & I64 & ", " & W32 & ", " & B32, "", Dummy);
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       FV  : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off), Inst_T);
                                       FrV : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       AfV : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                                       ExV : constant Expr_Val := Emit_Expr (Ctx, Args (4 + Off));
                                       F64 : Unbounded_String := FV.Reg;
                                       Fr32 : constant String := Emit_Int_Convert (Ctx, To_String (FrV.Reg), To_String (FrV.Pir_Type), "i32");
                                       Af32 : constant String := Emit_Int_Convert (Ctx, To_String (AfV.Reg), To_String (AfV.Pir_Type), "i32");
                                       Ex32 : constant String := Emit_Int_Convert (Ctx, To_String (ExV.Reg), To_String (ExV.Pir_Type), "i32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (FV.Reg), To_String (FV.Pir_Type), Inst_T);
                                       Emit_Field_Check (Fr32);
                                       Emit_Field_Check (Af32);
                                       Emit_Field_Check (Ex32);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Fl : constant String := Next_Reg (Ctx.E, "fxf");
                                             Sc : constant String := Next_Reg (Ctx.E, "fxs");
                                             Sm_T : constant TREE := (if not Is_Void_Tree (Inst_T) then Inst_T else FV.Ada_Type);
                                          begin
                                             Emit_Inst (Ctx.E, Fl & " = sitofp.f64 " & To_String (FV.Reg));
                                             Emit_Inst (Ctx.E, Sc & " = fmul.f64 " & Fl & ", " & Format_Float64 (Fixed_Small (Sm_T)));
                                             F64 := To_Unbounded_String (Sc);
                                          end;
                                       elsif To_String (FV.Pir_Type) = "f32" then
                                          F64 := To_Unbounded_String (Emit_Scalar_Coerce (Ctx, To_String (FV.Reg), "f32", "f64"));
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_f64",
                                         FP & ", " & To_String (F64) & ", " & Fr32 & ", " & Af32 & ", " & Ex32, "", Dummy);
                                    end;
                                 else
                                    -- ENUMERATION_IO.PUT([FILE,] ITEM, WIDTH, SET)
                                    declare
                                       EV   : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
                                       WV   : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       SetV : constant Expr_Val := Emit_Expr (Ctx, Args (3 + Off));
                                       BT   : constant TREE := Base_Type_Spec (EV.Ada_Type);
                                       PS   : Unbounded_String;
                                       Cnt  : Natural;
                                       Idx64 : constant String := Emit_Int_Convert (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), "i64", Is_Unsigned => True);
                                       W32   : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       PA    : constant String := Next_Reg (Ctx.E, "epa");
                                       PE    : constant String := Next_Reg (Ctx.E, "epe");
                                       SP    : constant String := Next_Reg (Ctx.E, "esp");
                                       LC    : constant String := Next_Reg (Ctx.E, "elc");
                                       LC32  : constant String := Next_Reg (Ctx.E, "elc32");
                                    begin
                                       Emit_Range_Check (Ctx, To_String (EV.Reg), To_String (EV.Pir_Type), Inst_T);
                                       Emit_Field_Check (W32);
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       pragma Unreferenced (Cnt);
                                       Emit_Inst (Ctx.E, PA & " = addr " & To_String (PS));
                                       Emit_Inst (Ctx.E, PE & " = elem_ptr ptr, " & PA & ", " & Idx64);
                                       Emit_Inst (Ctx.E, SP & " = load.ptr " & PE & ", align 8");
                                       Emit_Inst (Ctx.E, LC & " = cmp_eq." & To_String (SetV.Pir_Type) & " " & To_String (SetV.Reg) & ", 0");
                                       Emit_Inst (Ctx.E, LC32 & " = zext.i32 " & LC);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_enum",
                                         FP & ", " & SP & ", " & W32 & ", " & LC32, "", Dummy);
                                    end;
                                 end if;
                              end;
                              return Res;
                           end if;
                        elsif Name = "GET" then
                           Handled := True;
                           if Is_Str_Arg (Args (1)) then
                              -- GET(FROM : in STRING; ITEM : out ...; LAST : out POSITIVE) (P3-6.13)
                              declare
                                 Src   : constant Expr_Val := Emit_Expr (Ctx, Args (1));
                                 SP, SF, SL : Unbounded_String;
                                 ItTgt : constant RTL_Out_Target := Resolve_RTL_Out_Target (Args (2));
                                 LaTgt : constant RTL_Out_Target := Resolve_RTL_Out_Target (Args (3));
                                 Inst_TI : constant Type_Info := Get_Type_Info (Ctx, Inst_T);
                                 LaTmp : constant String := Next_Reg (Ctx.E, "lat");
                                 LaVal : constant String := Next_Reg (Ctx.E, "lav");
                              begin
                                 Emit_Entry_Alloca (Ctx.E, LaTmp & " = alloca i32, align 4");
                                 Emit_Str_Ptr_Len (Src, SP, SF, SL);
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       RV : Unbounded_String;
                                    begin
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_i64_str",
                                         To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & LaTmp, "i64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i64", Inst_T);
                                       Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                       Emit_RTL_Out_Store (LaTgt, LaVal, "i32", CONST_VOID);
                                       Emit_RTL_Out_Store (ItTgt, To_String (RV), "i64", Inst_T);
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       RV : Unbounded_String;
                                    begin
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_f64_str",
                                         To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & LaTmp, "f64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "f64", Inst_T);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Dv  : constant String := Next_Reg (Ctx.E, "fxd");
                                             Ge  : constant String := Next_Reg (Ctx.E, "fxge");
                                             Hlf : constant String := Next_Reg (Ctx.E, "fxhl");
                                             Rnd : constant String := Next_Reg (Ctx.E, "fxrn");
                                             Cv  : constant String := Next_Reg (Ctx.E, "fxc");
                                          begin
                                             Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & To_String (RV) & ", " & Format_Float64 (Fixed_Small (Inst_T)));
                                             Emit_Inst (Ctx.E, Ge & " = fcmp_oge.f64 " & Dv & ", 0.0");
                                             Emit_Inst (Ctx.E, Hlf & " = select.f64 " & Ge & ", 0.5, -0.5");
                                             Emit_Inst (Ctx.E, Rnd & " = fadd.f64 " & Dv & ", " & Hlf);
                                             Emit_Inst (Ctx.E, Cv & " = fptosi." & To_String (Inst_TI.Pir_Type) & " " & Rnd & " [sat]");
                                             Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                             Emit_RTL_Out_Store (LaTgt, LaVal, "i32", CONST_VOID);
                                             Emit_RTL_Out_Store (ItTgt, Cv, To_String (Inst_TI.Pir_Type), Inst_T);
                                          end;
                                       else
                                          Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                          Emit_RTL_Out_Store (LaTgt, LaVal, "i32", CONST_VOID);
                                          Emit_RTL_Out_Store (ItTgt, To_String (RV), "f64", Inst_T);
                                       end if;
                                    end;
                                 else
                                    declare
                                       BT  : constant TREE := Base_Type_Spec (Inst_T);
                                       PS  : Unbounded_String;
                                       Cnt : Natural;
                                       TA  : constant String := Next_Reg (Ctx.E, "eta");
                                       RV  : Unbounded_String;
                                       IsCh : constant String := (if Is_Character_Type (BT) then "1" else "0");
                                    begin
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       Emit_Inst (Ctx.E, TA & " = addr " & To_String (PS));
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_enum_str",
                                         To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & TA & ", " & Img (Cnt) & ", " & LaTmp & ", " & IsCh, "i32", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i32", Inst_T);
                                       Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
                                       Emit_RTL_Out_Store (LaTgt, LaVal, "i32", CONST_VOID);
                                       Emit_RTL_Out_Store (ItTgt, To_String (RV), "i32", Inst_T);
                                    end;
                                 end if;
                              end;
                              return Res;
                           else
                              -- GET([FILE,] ITEM [, WIDTH]) (P3-6.13)
                              declare
                                 Has_F : constant Boolean := Is_File_Arg (Args (1));
                                 Off   : constant Natural := (if Has_F then 1 else 0);
                                 FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
                                 ItTgt : constant RTL_Out_Target := Resolve_RTL_Out_Target (Args (1 + Off));
                                 Inst_TI : constant Type_Info := Get_Type_Info (Ctx, Inst_T);
                              begin
                                 if GP_N = "INTEGER_IO" then
                                    declare
                                       WV  : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       W32 : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       RV  : Unbounded_String;
                                    begin
                                       Emit_Field_Check (W32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_i64", FP & ", " & W32, "i64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i64", Inst_T);
                                       Emit_RTL_Out_Store (ItTgt, To_String (RV), "i64", Inst_T);
                                    end;
                                 elsif GP_N = "FLOAT_IO" or else GP_N = "FIXED_IO" then
                                    declare
                                       WV  : constant Expr_Val := Emit_Expr (Ctx, Args (2 + Off));
                                       W32 : constant String := Emit_Int_Convert (Ctx, To_String (WV.Reg), To_String (WV.Pir_Type), "i32");
                                       RV  : Unbounded_String;
                                    begin
                                       Emit_Field_Check (W32);
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_f64", FP & ", " & W32, "f64", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "f64", Inst_T);
                                       if GP_N = "FIXED_IO" then
                                          declare
                                             Dv  : constant String := Next_Reg (Ctx.E, "fxd");
                                             Ge  : constant String := Next_Reg (Ctx.E, "fxge");
                                             Hlf : constant String := Next_Reg (Ctx.E, "fxhl");
                                             Rnd : constant String := Next_Reg (Ctx.E, "fxrn");
                                             Cv  : constant String := Next_Reg (Ctx.E, "fxc");
                                          begin
                                             Emit_Inst (Ctx.E, Dv & " = fdiv.f64 " & To_String (RV) & ", " & Format_Float64 (Fixed_Small (Inst_T)));
                                             Emit_Inst (Ctx.E, Ge & " = fcmp_oge.f64 " & Dv & ", 0.0");
                                             Emit_Inst (Ctx.E, Hlf & " = select.f64 " & Ge & ", 0.5, -0.5");
                                             Emit_Inst (Ctx.E, Rnd & " = fadd.f64 " & Dv & ", " & Hlf);
                                             Emit_Inst (Ctx.E, Cv & " = fptosi." & To_String (Inst_TI.Pir_Type) & " " & Rnd & " [sat]");
                                             Emit_RTL_Out_Store (ItTgt, Cv, To_String (Inst_TI.Pir_Type), Inst_T);
                                          end;
                                       else
                                          Emit_RTL_Out_Store (ItTgt, To_String (RV), "f64", Inst_T);
                                       end if;
                                    end;
                                 else
                                    declare
                                       BT  : constant TREE := Base_Type_Spec (Inst_T);
                                       PS  : Unbounded_String;
                                       Cnt : Natural;
                                       TA   : constant String := Next_Reg (Ctx.E, "eta");
                                       RV   : Unbounded_String;
                                       IsCh : constant String := (if Is_Character_Type (BT) then "1" else "0");
                                    begin
                                       Ensure_Enum_Tables_Pkg (Ctx, BT, PS, Cnt);
                                       Emit_Inst (Ctx.E, TA & " = addr " & To_String (PS));
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_enum", FP & ", " & TA & ", " & Img (Cnt) & ", " & IsCh, "i32", RV);
                                       Emit_Inst_Range_Data_Check (To_String (RV), "i32", Inst_T);
                                       Emit_RTL_Out_Store (ItTgt, To_String (RV), "i32", Inst_T);
                                    end;
                                 end if;
                              end;
                              return Res;
                           end if;
                        end if;
                        end;

                     elsif (GP_N = "SEQUENTIAL_IO" or else GP_N = "DIRECT_IO")
                       and then not Is_Void_Tree (Gen_P) and then Is_Library_Level (Gen_P)
                     then
                        -- Extract ELEMENT_TYPE byte size from instantiation SM_DECL_S
                        declare
                           Is_Dir  : constant Boolean := GP_N = "DIRECT_IO";
                           Kind_I  : constant String := (if Is_Dir then "3" else "2");
                           Elem_Sz : Natural := 8;
                           Bio_Elem_T : TREE := CONST_VOID;
                           Is_Uncon_Arr : Boolean := False;
                           Arr_NDims    : Natural := 1;
                           Arr_Comp_Sz  : Natural := 1;
                           DS      : constant TREE := D (SM_DECL_S, PUD);

                           function Emit_Bio_Item_Size (V : Expr_Val) return String is
                           begin
                              if not Is_Uncon_Arr then
                                 return Img (Elem_Sz);
                              end if;
                              declare
                                 Total : Unbounded_String := To_Unbounded_String (Next_Reg (Ctx.E, "bsz0"));
                              begin
                                 Emit_Inst (Ctx.E, To_String (Total) & " = const.i64 " & Img (Arr_Comp_Sz));
                                 for D_Idx in 1 .. Arr_NDims loop
                                    declare
                                       F, L : Unbounded_String;
                                       Cl64 : Unbounded_String;
                                       Nx   : constant String := Next_Reg (Ctx.E, "bnx");
                                    begin
                                       Get_Array_Dim_Bounds (Ctx, V, D_Idx, F, L);
                                       Cl64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F), To_String (L)));
                                       Emit_Inst (Ctx.E, Nx & " = mul.i64 " & To_String (Total) & ", " & To_String (Cl64));
                                       Total := To_Unbounded_String (Nx);
                                    end;
                                 end loop;
                                 return To_String (Total);
                              end;
                           end Emit_Bio_Item_Size;
                        begin
                           if not Is_Void_Tree (DS) and then not IS_EMPTY (LIST (DS)) then
                              declare
                                 D1 : constant TREE := HEAD (LIST (DS));
                              begin
                                 if KIND (D1) = DN_SUBTYPE_DECL then
                                    declare
                                       ET : constant TREE := Subst_Tree (Ctx, D (SM_TYPE_SPEC, D (AS_SOURCE_NAME, D1)));
                                       RT : constant TREE := Resolve_Type_Spec (ET);
                                       ETI : constant Type_Info := Get_Type_Info (Ctx, ET);
                                    begin
                                       Elem_Sz := Natural'Max (1, ETI.Byte_Size);
                                       Bio_Elem_T := ET;
                                       if not Is_Void_Tree (RT) and then KIND (RT) = DN_ARRAY then
                                          Is_Uncon_Arr := True;
                                          Arr_NDims := Natural'Max (1, ETI.NDims);
                                          declare
                                             CT : constant TREE := Resolve_Type_Spec (D (SM_COMP_TYPE, RT));
                                             CTI : constant Type_Info := Get_Type_Info (Ctx, CT);
                                          begin
                                             Arr_Comp_Sz := Natural'Max (1, CTI.Byte_Size);
                                          end;
                                       end if;
                                    end;
                                 end if;
                              end;
                           end if;

                           Handled := True;
                           if Name = "CREATE" or else Name = "OPEN" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                 M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
                                 NV  : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                 FoV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
                                 NP, NF, NL, FoP, FoF, FoL : Unbounded_String;
                                 Dummy : Unbounded_String;
                              begin
                                 Emit_Str_Ptr_Len (NV, NP, NF, NL);
                                 Emit_Str_Ptr_Len (FoV, FoP, FoF, FoL);
                                 pragma Unreferenced (NF, FoF);
                                 if Name = "CREATE" then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_bio_create",
                                      To_String (FLV.Reg) & ", " & Kind_I & ", " & M32 & ", " &
                                      To_String (NP) & ", " & To_String (NL) & ", " & To_String (FoP) & ", " & To_String (FoL) & ", " & Img (Elem_Sz), "", Dummy);
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_bio_open",
                                      To_String (FLV.Reg) & ", " & Kind_I & ", " & M32 & ", " &
                                      To_String (NP) & ", " & To_String (NL) & ", " & To_String (FoP) & ", " & To_String (FoL) & ", " & Img (Elem_Sz), "", Dummy);
                                 end if;
                              end;
                           elsif Name = "CLOSE" or else Name = "DELETE" or else Name = "RESET" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 Dummy : Unbounded_String;
                              begin
                                 if Name = "CLOSE" then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_tio_close", To_String (FLV.Reg), "", Dummy);
                                 elsif Name = "DELETE" then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_tio_delete", To_String (FLV.Reg), "", Dummy);
                                 elsif NArg = 2 then
                                    declare
                                       MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                       M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
                                    begin
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset_mode", To_String (FLV.Reg) & ", " & M32, "", Dummy);
                                    end;
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset", To_String (FLV.Reg), "", Dummy);
                                 end if;
                              end;
                           elsif Name = "IS_OPEN" or else Name = "MODE" or else Name = "END_OF_FILE" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 Fn  : constant String :=
                                   (if Name = "IS_OPEN" then "@__ada_tio_is_open"
                                    elsif Name = "MODE" then "@__ada_tio_mode"
                                    elsif Is_Dir then "@__ada_dir_eof"
                                    else "@__ada_seq_eof");
                                 RV  : Unbounded_String;
                              begin
                                 Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "i8", RV);
                                 Res.Kind := EK_Scalar; Res.Reg := RV;
                                 Res.Pir_Type := To_Unbounded_String ("i8");
                                 Res.Ada_Type := Res_Type; Res.Align := 1;
                              end;
                           elsif Name = "NAME" or else Name = "FORM" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 LP  : constant String := Next_Reg (Ctx.E, "nflp");
                                 LV  : constant String := Next_Reg (Ctx.E, "nfl");
                                 One : constant String := Next_Reg (Ctx.E, "nf1");
                                 Fn  : constant String := (if Name = "NAME" then "@__ada_tio_name" else "@__ada_tio_form");
                                 RV  : Unbounded_String;
                              begin
                                 Emit_Entry_Alloca (Ctx.E, LP & " = alloca i32, align 4");
                                 Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg) & ", " & LP, "ptr", RV);
                                 Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
                                 Emit_Inst (Ctx.E, One & " = const.i64 1");
                                 Res.Kind := EK_Slice; Res.Reg := RV; Res.Pir_Type := To_Unbounded_String ("ptr");
                                 Res.Ada_Type := Res_Type; Res.Align := 1; Res.NDims := 1;
                                 Res.First_Regs (1) := To_Unbounded_String (One);
                                 Res.Last_Regs (1)  := To_Unbounded_String (Emit_Int_Convert (Ctx, LV, "i32", "i64"));
                              end;
                           elsif Name = "READ" then
                              declare
                                 FLV  : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 BTI  : constant Type_Info := Get_Type_Info (Ctx, Bio_Elem_T);
                                 Is_Scalar_Bio : constant Boolean := (not Is_Uncon_Arr) and then BTI.Kind = TK_Scalar;
                                 ItTgt : RTL_Out_Target;
                                 ItLV  : Expr_Val;
                                 Read_Ptr : Unbounded_String;
                                 Sz64 : Unbounded_String;
                                 Need_Comp_Tmp : Boolean := False;
                                 Dummy : Unbounded_String;
                              begin
                                 if Is_Scalar_Bio then
                                    ItTgt := Resolve_RTL_Out_Target (Args (2));
                                    declare
                                       Tmp : constant String := Next_Reg (Ctx.E, "bior");
                                    begin
                                       Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (BTI.Pir_Type) & ", align " & Img (Natural'Max (1, BTI.Align)));
                                       Read_Ptr := To_Unbounded_String (Tmp);
                                    end;
                                    Sz64 := To_Unbounded_String (Img (Elem_Sz));
                                 else
                                    ItLV := Emit_LValue (Ctx, Args (2));
                                    Sz64 := To_Unbounded_String (Emit_Bio_Item_Size (ItLV));
                                    -- P3-6.12: Read composite record into temp buffer first so failed discriminant check never corrupts Item!
                                    if BTI.Kind = TK_Record and then not Is_Uncon_Arr and then Elem_Sz > 0 then
                                       declare
                                          Tmp : constant String := Next_Reg (Ctx.E, "biorc");
                                       begin
                                          Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca [" & Img (Elem_Sz) & " x i8], align " & Img (Natural'Max (1, BTI.Align)));
                                          Read_Ptr := To_Unbounded_String (Tmp);
                                          Need_Comp_Tmp := True;
                                       end;
                                    else
                                       Read_Ptr := ItLV.Reg;
                                    end if;
                                    -- P3-6.12: Check target constrained array length matches Bio_Elem_T
                                    if BTI.Kind = TK_Constrained_Array and then not Is_Uncon_Arr and then Ctx.Enable_Checks then
                                       declare
                                          Tgt_A : Expr_Val;
                                          ND_B  : constant Natural := Natural'Max (1, Natural'Min (BTI.NDims, Max_Array_Dims));
                                       begin
                                          Tgt_A.Kind := EK_Slice; Tgt_A.Ada_Type := Bio_Elem_T; Tgt_A.NDims := ND_B;
                                          for D_I in 1 .. ND_B loop
                                             Emit_Index_Bounds (Ctx, BTI.Dims (D_I).Idx_Type, Tgt_A.First_Regs (D_I), Tgt_A.Last_Regs (D_I));
                                          end loop;
                                          Emit_Array_Length_Check (Ctx, Tgt_A, ItLV, ND_B);
                                       end;
                                    end if;
                                 end if;
                                 if Is_Dir then
                                    declare
                                       Fr64   : Unbounded_String := To_Unbounded_String ("0");
                                       HasPos : constant String := (if NArg = 3 then "1" else "0");
                                    begin
                                       if NArg = 3 then
                                          declare
                                             FrV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                          begin
                                             Fr64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (FrV.Reg), To_String (FrV.Pir_Type), "i64"));
                                          end;
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_dir_read",
                                         To_String (FLV.Reg) & ", " & To_String (Read_Ptr) & ", " & To_String (Sz64) & ", " & To_String (Fr64) & ", " & HasPos, "", Dummy);
                                    end;
                                 elsif Is_Uncon_Arr and then Arr_NDims > 1 then
                                    -- P3-6.18: Frame all NDims lengths for multi-D unconstrained arrays!
                                    declare
                                       LensP : constant String := Next_Reg (Ctx.E, "blns");
                                    begin
                                       Emit_Entry_Alloca (Ctx.E, LensP & " = alloca [" & Img (Arr_NDims) & " x i64], align 8");
                                       for D_I in 1 .. Arr_NDims loop
                                          declare
                                             F_D, L_D : Unbounded_String;
                                             DL64 : Unbounded_String;
                                             EP_L : constant String := Next_Reg (Ctx.E, "blep");
                                          begin
                                             Get_Array_Dim_Bounds (Ctx, ItLV, D_I, F_D, L_D);
                                             DL64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_D), To_String (L_D)));
                                             Emit_Inst (Ctx.E, EP_L & " = elem_ptr i64, " & LensP & ", " & Img (D_I - 1));
                                             Emit_Inst (Ctx.E, "store.i64 " & To_String (DL64) & ", " & EP_L & ", align 8");
                                          end;
                                       end loop;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_seq_read_framed_nd",
                                         To_String (FLV.Reg) & ", " & To_String (Read_Ptr) & ", " & Img (Arr_NDims) & ", " & LensP & ", " & To_String (Sz64), "", Dummy);
                                    end;
                                 elsif Is_Uncon_Arr then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_read_framed",
                                      To_String (FLV.Reg) & ", " & To_String (Read_Ptr) & ", " & To_String (Sz64), "", Dummy);
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_read",
                                      To_String (FLV.Reg) & ", " & To_String (Read_Ptr) & ", " & To_String (Sz64), "", Dummy);
                                 end if;
                                 if Is_Scalar_Bio then
                                    declare
                                       RV : constant String := Next_Reg (Ctx.E, "biov");
                                    begin
                                       Emit_Inst (Ctx.E, RV & " = load." & To_String (BTI.Pir_Type) & " " & To_String (Read_Ptr) & ", align " & Img (Natural'Max (1, BTI.Align)));
                                       -- P3-6.11: ELEMENT_TYPE violation raises DATA_ERROR (LRM 14.2.2(6), 14.2.4(7))
                                       Emit_Inst_Range_Data_Check_RTL (RV, To_String (BTI.Pir_Type), Bio_Elem_T);
                                       -- P3-6.13: Item's own subtype check raises CONSTRAINT_ERROR + supports DN_CONVERSION & bitfields
                                       Emit_RTL_Out_Store (ItTgt, RV, To_String (BTI.Pir_Type), Bio_Elem_T);
                                    end;
                                 elsif Need_Comp_Tmp then
                                    -- P3-6.12: Check record discriminants against Bio_Elem_T and Item's subtype before copying into Item!
                                    Emit_Discriminant_Check (Ctx, To_String (Read_Ptr), Bio_Elem_T, Is_Access => False);
                                    Emit_Discriminant_Check (Ctx, To_String (Read_Ptr), ItLV.Ada_Type, Is_Access => False);
                                    Emit_Inst (Ctx.E, "memcpy " & To_String (ItLV.Reg) & ", " & To_String (Read_Ptr) & ", " & Img (Elem_Sz) & ", align 1");
                                 end if;
                              end;
                           elsif Name = "WRITE" then
                              declare
                                 FLV  : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 BTI  : constant Type_Info := Get_Type_Info (Ctx, Bio_Elem_T);
                                 ItV  : constant Expr_Val := Emit_Expr (Ctx, Args (2), Bio_Elem_T);
                                 Sz64 : constant String := Emit_Bio_Item_Size (ItV);
                                 ItP  : Unbounded_String := ItV.Reg;
                                 Dummy : Unbounded_String;
                              begin
                                 -- P3-6.12: Enforce Bio_Elem_T subtype/length/discriminant checks & scalar coercion on WRITE!
                                 if ItV.Kind = EK_Scalar and then BTI.Kind = TK_Scalar then
                                    declare
                                       VR  : constant String := Emit_Scalar_Coerce (Ctx, To_String (ItV.Reg), To_String (ItV.Pir_Type), To_String (BTI.Pir_Type),
                                         Is_Unsigned => Is_Unsigned_Scalar (ItV.Ada_Type, To_String (ItV.Pir_Type)));
                                       Tmp : constant String := Next_Reg (Ctx.E, "biow");
                                    begin
                                       if Int_Bits_Of_Pir (To_String (ItV.Pir_Type)) > Int_Bits_Of_Pir (To_String (BTI.Pir_Type))
                                         and then Is_Discrete_Type (Bio_Elem_T)
                                       then
                                          Emit_Range_Check (Ctx, To_String (ItV.Reg), To_String (ItV.Pir_Type), Bio_Elem_T);
                                       end if;
                                       Emit_Range_Check (Ctx, VR, To_String (BTI.Pir_Type), Bio_Elem_T);
                                       Emit_Discriminant_Check (Ctx, VR, Bio_Elem_T, Is_Access => True);
                                       Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (BTI.Pir_Type) & ", align " & Img (Natural'Max (1, BTI.Align)));
                                       Emit_Inst (Ctx.E, "store." & To_String (BTI.Pir_Type) & " " & VR & ", " & Tmp & ", align " & Img (Natural'Max (1, BTI.Align)));
                                       ItP := To_Unbounded_String (Tmp);
                                    end;
                                 elsif BTI.Kind = TK_Record then
                                    Emit_Discriminant_Check (Ctx, To_String (ItV.Reg), Bio_Elem_T, Is_Access => False);
                                 elsif BTI.Kind = TK_Constrained_Array and then not Is_Uncon_Arr and then Ctx.Enable_Checks then
                                    declare
                                       Tgt_A : Expr_Val;
                                       ND_B  : constant Natural := Natural'Max (1, Natural'Min (BTI.NDims, Max_Array_Dims));
                                    begin
                                       Tgt_A.Kind := EK_Slice; Tgt_A.Ada_Type := Bio_Elem_T; Tgt_A.NDims := ND_B;
                                       for D_I in 1 .. ND_B loop
                                          Emit_Index_Bounds (Ctx, BTI.Dims (D_I).Idx_Type, Tgt_A.First_Regs (D_I), Tgt_A.Last_Regs (D_I));
                                       end loop;
                                       Emit_Array_Length_Check (Ctx, Tgt_A, ItV, ND_B);
                                    end;
                                 end if;
                                 if Is_Dir then
                                    declare
                                       To64   : Unbounded_String := To_Unbounded_String ("0");
                                       HasPos : constant String := (if NArg = 3 then "1" else "0");
                                    begin
                                       if NArg = 3 then
                                          declare
                                             ToV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
                                          begin
                                             To64 := To_Unbounded_String (Emit_Int_Convert (Ctx, To_String (ToV.Reg), To_String (ToV.Pir_Type), "i64"));
                                          end;
                                       end if;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_dir_write",
                                         To_String (FLV.Reg) & ", " & To_String (ItP) & ", " & Sz64 & ", " & To_String (To64) & ", " & HasPos, "", Dummy);
                                    end;
                                 elsif Is_Uncon_Arr and then Arr_NDims > 1 then
                                    declare
                                       LensP : constant String := Next_Reg (Ctx.E, "blns");
                                    begin
                                       Emit_Entry_Alloca (Ctx.E, LensP & " = alloca [" & Img (Arr_NDims) & " x i64], align 8");
                                       for D_I in 1 .. Arr_NDims loop
                                          declare
                                             F_D, L_D : Unbounded_String;
                                             DL64 : Unbounded_String;
                                             EP_L : constant String := Next_Reg (Ctx.E, "blep");
                                          begin
                                             Get_Array_Dim_Bounds (Ctx, ItV, D_I, F_D, L_D);
                                             DL64 := To_Unbounded_String (Emit_Dim_Len_I64 (Ctx, To_String (F_D), To_String (L_D)));
                                             Emit_Inst (Ctx.E, EP_L & " = elem_ptr i64, " & LensP & ", " & Img (D_I - 1));
                                             Emit_Inst (Ctx.E, "store.i64 " & To_String (DL64) & ", " & EP_L & ", align 8");
                                          end;
                                       end loop;
                                       Emit_Call_Or_Invoke (Ctx, "@__ada_seq_write_framed_nd",
                                         To_String (FLV.Reg) & ", " & To_String (ItP) & ", " & Img (Arr_NDims) & ", " & LensP & ", " & Sz64, "", Dummy);
                                    end;
                                 elsif Is_Uncon_Arr then
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_write_framed",
                                      To_String (FLV.Reg) & ", " & To_String (ItP) & ", " & Sz64, "", Dummy);
                                 else
                                    Emit_Call_Or_Invoke (Ctx, "@__ada_seq_write",
                                      To_String (FLV.Reg) & ", " & To_String (ItP) & ", " & Sz64, "", Dummy);
                                 end if;
                              end;
                           elsif Name = "SET_INDEX" then
                              declare
                                 FLV  : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 ToV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                                 To64 : constant String := Emit_Int_Convert (Ctx, To_String (ToV.Reg), To_String (ToV.Pir_Type), "i64");
                                 Dummy : Unbounded_String;
                              begin
                                 Emit_Call_Or_Invoke (Ctx, "@__ada_dir_set_index", To_String (FLV.Reg) & ", " & To64, "", Dummy);
                              end;
                           elsif Name = "INDEX" or else Name = "SIZE" then
                              declare
                                 FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
                                 Fn  : constant String := (if Name = "INDEX" then "@__ada_dir_index" else "@__ada_dir_size");
                                 RV  : Unbounded_String;
                              begin
                                 Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "i32", RV);
                                 Res.Kind := EK_Scalar; Res.Reg := RV;
                                 Res.Pir_Type := To_Unbounded_String ("i32");
                                 Res.Ada_Type := Res_Type; Res.Align := 4;
                              end;
                           else
                              Handled := False;
                           end if;
                           if Handled then
                              return Res;
                           end if;
                        end;
                     end if;
                  end;
               end if;
            end;
         end if;
      end;

      if Pkg = "LOW_LEVEL_IO" and then Is_Library_Level (Def) then
         Handled := True;
         if Name = "SEND_CONTROL" or else Name = "RECEIVE_CONTROL" then
            declare
               DevV  : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               Dev32 : constant String := Emit_Int_Convert (Ctx, To_String (DevV.Reg), To_String (DevV.Pir_Type), "i32");
               Fn    : constant String := (if Name = "SEND_CONTROL" then "@__ada_llio_send" else "@__ada_llio_recv");
               Dummy : Unbounded_String;
            begin
               if Name = "RECEIVE_CONTROL" then
                  -- P3-6.13: Handle scalar view conversions & bitfields on RECEIVE_CONTROL OUT actual
                  declare
                     DT_LL : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Args (2)));
                     DTI_LL : constant Type_Info := Get_Type_Info (Ctx, DT_LL);
                  begin
                     if DTI_LL.Kind = TK_Scalar then
                        declare
                           Tgt : constant RTL_Out_Target := Resolve_RTL_Out_Target (Args (2));
                           Tmp : constant String := Next_Reg (Ctx.E, "llrt");
                           Val : constant String := Next_Reg (Ctx.E, "llrv");
                        begin
                           Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (DTI_LL.Pir_Type) & ", align " & Img (Natural'Max (1, DTI_LL.Align)));
                           Emit_Inst (Ctx.E, "memset " & Tmp & ", 0, " & Img (Natural'Max (1, DTI_LL.Byte_Size)) & ", align 1");
                           Emit_Call_Or_Invoke (Ctx, Fn, Dev32 & ", " & Tmp, "", Dummy, Nounwind => True);
                           Emit_Inst (Ctx.E, Val & " = load." & To_String (DTI_LL.Pir_Type) & " " & Tmp & ", align " & Img (Natural'Max (1, DTI_LL.Align)));
                           Emit_RTL_Out_Store (Tgt, Val, To_String (DTI_LL.Pir_Type), DT_LL);
                        end;
                     else
                        declare
                           DatLV : constant Expr_Val := Emit_LValue (Ctx, Args (2));
                        begin
                           Emit_Call_Or_Invoke (Ctx, Fn, Dev32 & ", " & To_String (DatLV.Reg), "", Dummy, Nounwind => True);
                        end;
                     end if;
                  end;
               else
                  declare
                     DatLV : constant Expr_Val := Emit_LValue (Ctx, Args (2));
                  begin
                     Emit_Call_Or_Invoke (Ctx, Fn, Dev32 & ", " & To_String (DatLV.Reg), "", Dummy, Nounwind => True);
                  end;
               end if;
            end;
         else
            Handled := False;
         end if;
         if Handled then
            return Res;
         end if;
      end if;

      if Pkg = "TEXT_IO" and then Is_Library_Level (Def) then
         Handled := True;
         if Name = "CREATE" or else Name = "OPEN" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
               M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
               NV  : constant Expr_Val := Emit_Expr (Ctx, Args (3));
               FoV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
               NP, NF, NL, FoP, FoF, FoL : Unbounded_String;
               Fn  : constant String := (if Name = "CREATE" then "@__ada_tio_create" else "@__ada_tio_open");
               Dummy : Unbounded_String;
            begin
               Emit_Str_Ptr_Len (NV, NP, NF, NL);
               Emit_Str_Ptr_Len (FoV, FoP, FoF, FoL);
               pragma Unreferenced (NF, FoF);
               Emit_Call_Or_Invoke (Ctx, Fn,
                 To_String (FLV.Reg) & ", " & M32 & ", " &
                 To_String (NP) & ", " & To_String (NL) & ", " &
                 To_String (FoP) & ", " & To_String (FoL), "", Dummy);
            end;
         elsif Name = "CLOSE" or else Name = "DELETE" or else Name = "RESET" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               Dummy : Unbounded_String;
            begin
               if Name = "CLOSE" then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_close", To_String (FLV.Reg), "", Dummy);
               elsif Name = "DELETE" then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_delete", To_String (FLV.Reg), "", Dummy);
               elsif NArg = 2 then
                  declare
                     MV  : constant Expr_Val := Emit_Expr (Ctx, Args (2));
                     M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
                  begin
                     Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset_mode", To_String (FLV.Reg) & ", " & M32, "", Dummy);
                  end;
               else
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_reset", To_String (FLV.Reg), "", Dummy);
               end if;
            end;
         elsif Name = "IS_OPEN" or else Name = "MODE" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               Fn  : constant String := (if Name = "IS_OPEN" then "@__ada_tio_is_open" else "@__ada_tio_mode");
               RV  : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "i8", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Res_Type; Res.Align := 1;
            end;
         elsif Name = "NAME" or else Name = "FORM" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               LP  : constant String := Next_Reg (Ctx.E, "nflp");
               LV  : constant String := Next_Reg (Ctx.E, "nfl");
               One : constant String := Next_Reg (Ctx.E, "nf1");
               Fn  : constant String := (if Name = "NAME" then "@__ada_tio_name" else "@__ada_tio_form");
               RV  : Unbounded_String;
            begin
               Emit_Entry_Alloca (Ctx.E, LP & " = alloca i32, align 4");
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg) & ", " & LP, "ptr", RV);
               Emit_Inst (Ctx.E, LV & " = load.i32 " & LP & ", align 4");
               Emit_Inst (Ctx.E, One & " = const.i64 1");
               Res.Kind := EK_Slice; Res.Reg := RV; Res.Pir_Type := To_Unbounded_String ("ptr");
               Res.Ada_Type := Res_Type; Res.Align := 1; Res.NDims := 1;
               Res.First_Regs (1) := To_Unbounded_String (One);
               Res.Last_Regs (1)  := To_Unbounded_String (Emit_Int_Convert (Ctx, LV, "i32", "i64"));
            end;
         elsif Name = "SET_INPUT" or else Name = "SET_OUTPUT" then
            declare
               FLV : constant Expr_Val := Emit_LValue (Ctx, Args (1));
               Fn  : constant String := (if Name = "SET_INPUT" then "@__ada_tio_set_input" else "@__ada_tio_set_output");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (FLV.Reg), "", Dummy);
            end;
         elsif Name = "STANDARD_OUTPUT" or else Name = "CURRENT_OUTPUT"
           or else Name = "STANDARD_INPUT" or else Name = "CURRENT_INPUT"
         then
            declare
               RTI : constant Type_Info := Get_Type_Info (Ctx, Res_Type);
               Tmp : constant String := Next_Reg (Ctx.E, "ft");
               FP  : constant String := Next_Reg (Ctx.E, "ftf");
               Fn  : constant String :=
                 (if Name = "STANDARD_INPUT" then "@__ada_tio_standard_input"
                  elsif Name = "STANDARD_OUTPUT" then "@__ada_tio_standard_output"
                  elsif Name = "CURRENT_INPUT" then "@__ada_tio_current_input"
                  else "@__ada_tio_current_output");
               HV  : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, "", "i32", HV, Nounwind => True);
               Emit_Entry_Alloca (Ctx.E, Tmp & " = alloca " & To_String (RTI.Pir_Type) & ", align " & Img (RTI.Align));
               Emit_Inst (Ctx.E, FP & " = field_ptr " & To_String (RTI.Pir_Type) & ", " & Tmp & ", 0");
               Emit_Inst (Ctx.E, "store.i32 " & To_String (HV) & ", " & FP & ", align 4");
               Res.Kind     := EK_Addr;
               Res.Reg      := To_Unbounded_String (Tmp);
               Res.Pir_Type := RTI.Pir_Type;
               Res.Ada_Type := Res_Type;
               Res.Align    := RTI.Align;
            end;
         elsif Name = "SET_LINE_LENGTH" or else Name = "SET_PAGE_LENGTH"
           or else Name = "SET_COL" or else Name = "SET_LINE"
           or else Name = "NEW_LINE" or else Name = "SKIP_LINE"
         then
            declare
               Has_F : constant Boolean := (NArg = 2);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               V     : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
               V32   : constant String := Emit_Int_Convert (Ctx, To_String (V.Reg), To_String (V.Pir_Type), "i32");
               Fn    : constant String :=
                 (if Name = "SET_LINE_LENGTH" then "@__ada_tio_set_line_length"
                  elsif Name = "SET_PAGE_LENGTH" then "@__ada_tio_set_page_length"
                  elsif Name = "SET_COL" then "@__ada_tio_set_col"
                  elsif Name = "SET_LINE" then "@__ada_tio_set_line"
                  elsif Name = "NEW_LINE" then "@__ada_tio_new_line"
                  else "@__ada_tio_skip_line");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP & ", " & V32, "", Dummy);
            end;
         elsif Name = "NEW_PAGE" or else Name = "SKIP_PAGE" then
            declare
               FP : constant String := Emit_File_Ptr_Or_Null (NArg = 1);
               Fn : constant String := (if Name = "NEW_PAGE" then "@__ada_tio_new_page" else "@__ada_tio_skip_page");
               Dummy : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP, "", Dummy);
            end;
         elsif Name = "LINE_LENGTH" or else Name = "PAGE_LENGTH"
           or else Name = "COL" or else Name = "LINE" or else Name = "PAGE"
         then
            declare
               FP : constant String := Emit_File_Ptr_Or_Null (NArg = 1);
               Fn : constant String :=
                 (if Name = "LINE_LENGTH" then "@__ada_tio_line_length"
                  elsif Name = "PAGE_LENGTH" then "@__ada_tio_page_length"
                  elsif Name = "COL" then "@__ada_tio_col"
                  elsif Name = "LINE" then "@__ada_tio_line"
                  else "@__ada_tio_page");
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP, "i32", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i32");
               Res.Ada_Type := Res_Type; Res.Align := 4;
            end;
         elsif Name = "END_OF_LINE" or else Name = "END_OF_PAGE" or else Name = "END_OF_FILE" then
            declare
               FP : constant String := Emit_File_Ptr_Or_Null (NArg = 1);
               Fn : constant String :=
                 (if Name = "END_OF_LINE" then "@__ada_tio_end_of_line"
                  elsif Name = "END_OF_PAGE" then "@__ada_tio_end_of_page"
                  else "@__ada_tio_end_of_file");
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, FP, "i8", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i8");
               Res.Ada_Type := Res_Type; Res.Align := 1;
            end;
         elsif Name = "PUT" or else Name = "PUT_LINE" then
            declare
               Has_F : constant Boolean := (NArg = 2);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               V     : constant Expr_Val := Emit_Expr (Ctx, Args (1 + Off));
               Dummy : Unbounded_String;
            begin
               if Is_Character_Type (V.Ada_Type) then
                  Emit_Call_Or_Invoke (Ctx, "@__ada_tio_put_char", FP & ", " & To_String (V.Reg), "", Dummy);
               else
                  declare
                     SP, SF, SL : Unbounded_String;
                     Fn : constant String := (if Name = "PUT_LINE" then "@__ada_tio_put_line" else "@__ada_tio_put_str");
                  begin
                     Emit_Str_Ptr_Len (V, SP, SF, SL);
                     pragma Unreferenced (SF);
                     Emit_Call_Or_Invoke (Ctx, Fn, FP & ", " & To_String (SP) & ", " & To_String (SL), "", Dummy);
                  end;
               end if;
            end;
         elsif Name = "GET" then
            declare
               Has_F : constant Boolean := (NArg = 2);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               Arg_T : constant TREE := Subst_Tree (Ctx, D (SM_EXP_TYPE, Args (1 + Off)));
               Dummy : Unbounded_String;
            begin
               if Is_Character_Type (Arg_T) then
                  -- P3-6.13 & P3-6.14: Support DN_CONVERSION, bitfields, and subtype range check on TEXT_IO.GET(Char)!
                  declare
                     ChTgt : constant RTL_Out_Target := Resolve_RTL_Out_Target (Args (1 + Off));
                     RV    : Unbounded_String;
                  begin
                     Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_char", FP, "i8", RV);
                     Emit_RTL_Out_Store (ChTgt, To_String (RV), "i8", CONST_VOID);
                  end;
               else
                  declare
                     LV : constant Expr_Val := Emit_LValue (Ctx, Args (1 + Off));
                     SP, SF, SL : Unbounded_String;
                  begin
                     Emit_Str_Ptr_Len (LV, SP, SF, SL);
                     pragma Unreferenced (SF);
                     Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_str", FP & ", " & To_String (SP) & ", " & To_String (SL), "", Dummy);
                  end;
               end if;
            end;
         elsif Name = "GET_LINE" then
            declare
               Has_F : constant Boolean := (NArg = 3);
               Off   : constant Natural := (if Has_F then 1 else 0);
               FP    : constant String := Emit_File_Ptr_Or_Null (Has_F);
               SLV   : constant Expr_Val := Emit_LValue (Ctx, Args (1 + Off));
               -- P3-6.13 & P3-6.14: Write LAST to temp i32 then store via Emit_RTL_Out_Store (subtype check + view conv + bitfield!)
               LaTgt : constant RTL_Out_Target := Resolve_RTL_Out_Target (Args (2 + Off));
               LaTmp : constant String := Next_Reg (Ctx.E, "gllt");
               LaVal : constant String := Next_Reg (Ctx.E, "gllv");
               SP, SF, SL : Unbounded_String;
               Dummy : Unbounded_String;
            begin
               Emit_Entry_Alloca (Ctx.E, LaTmp & " = alloca i32, align 4");
               Emit_Str_Ptr_Len (SLV, SP, SF, SL);
               Emit_Call_Or_Invoke (Ctx, "@__ada_tio_get_line",
                 FP & ", " & To_String (SP) & ", " & To_String (SF) & ", " & To_String (SL) & ", " & LaTmp, "", Dummy);
               Emit_Inst (Ctx.E, LaVal & " = load.i32 " & LaTmp & ", align 4");
               Emit_RTL_Out_Store (LaTgt, LaVal, "i32", CONST_VOID);
            end;
         else
            Handled := False;
         end if;
         if Handled then
            return Res;
         end if;
      elsif Pkg = "CALENDAR" and then Is_Library_Level (Def) then
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
         elsif Name = "YEAR" or else Name = "MONTH" or else Name = "DAY" then
            Handled := True;
            declare
               TV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               Fn : constant String :=
                 (if Name = "YEAR" then "@__ada_cal_year"
                  elsif Name = "MONTH" then "@__ada_cal_month"
                  else "@__ada_cal_day");
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, Fn, To_String (TV.Reg), "i32", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i32");
               Res.Ada_Type := Res_Type; Res.Align := 4;
            end;
            return Res;
         elsif Name = "SECONDS" then
            Handled := True;
            declare
               TV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               RV : Unbounded_String;
            begin
               Emit_Call_Or_Invoke (Ctx, "@__ada_cal_seconds", To_String (TV.Reg), "i64", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i64");
               Res.Ada_Type := Res_Type; Res.Align := 8;
            end;
            return Res;
         elsif Name = "TIME_OF" then
            Handled := True;
            declare
               YV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               MV : constant Expr_Val := Emit_Expr (Ctx, Args (2));
               DV : constant Expr_Val := Emit_Expr (Ctx, Args (3));
               SV : constant Expr_Val := Emit_Expr (Ctx, Args (4));
               Y32 : constant String := Emit_Int_Convert (Ctx, To_String (YV.Reg), To_String (YV.Pir_Type), "i32");
               M32 : constant String := Emit_Int_Convert (Ctx, To_String (MV.Reg), To_String (MV.Pir_Type), "i32");
               D32 : constant String := Emit_Int_Convert (Ctx, To_String (DV.Reg), To_String (DV.Pir_Type), "i32");
               S64 : constant String := Emit_Int_Convert (Ctx, To_String (SV.Reg), To_String (SV.Pir_Type), "i64");
               RV  : Unbounded_String;
            begin
               -- Subtype range checks for YEAR_NUMBER (1901..2099), MONTH_NUMBER (1..12),
               -- DAY_NUMBER (1..31), DAY_DURATION (0.0..86400.0) raise CONSTRAINT_ERROR (LRM 9.6)
               if Ctx.Enable_Checks then
                  declare
                     YLo : constant String := Next_Reg (Ctx.E, "cy1");
                     YHi : constant String := Next_Reg (Ctx.E, "cy2");
                     YB  : constant String := Next_Reg (Ctx.E, "cyb");
                     MLo : constant String := Next_Reg (Ctx.E, "cm1");
                     MHi : constant String := Next_Reg (Ctx.E, "cm2");
                     MB  : constant String := Next_Reg (Ctx.E, "cmb");
                     DLo : constant String := Next_Reg (Ctx.E, "cd1");
                     DHi : constant String := Next_Reg (Ctx.E, "cd2");
                     DB  : constant String := Next_Reg (Ctx.E, "cdb");
                     SLo : constant String := Next_Reg (Ctx.E, "cs1");
                     SHi : constant String := Next_Reg (Ctx.E, "cs2");
                     SB  : constant String := Next_Reg (Ctx.E, "csb");
                     B1  : constant String := Next_Reg (Ctx.E, "cb1");
                     B2  : constant String := Next_Reg (Ctx.E, "cb2");
                     Bad : constant String := Next_Reg (Ctx.E, "cbad");
                  begin
                     Emit_Inst (Ctx.E, YLo & " = cmp_lt_s.i32 " & Y32 & ", 1901");
                     Emit_Inst (Ctx.E, YHi & " = cmp_gt_s.i32 " & Y32 & ", 2099");
                     Emit_Inst (Ctx.E, YB  & " = or.i8 " & YLo & ", " & YHi);
                     Emit_Inst (Ctx.E, MLo & " = cmp_lt_s.i32 " & M32 & ", 1");
                     Emit_Inst (Ctx.E, MHi & " = cmp_gt_s.i32 " & M32 & ", 12");
                     Emit_Inst (Ctx.E, MB  & " = or.i8 " & MLo & ", " & MHi);
                     Emit_Inst (Ctx.E, DLo & " = cmp_lt_s.i32 " & D32 & ", 1");
                     Emit_Inst (Ctx.E, DHi & " = cmp_gt_s.i32 " & D32 & ", 31");
                     Emit_Inst (Ctx.E, DB  & " = or.i8 " & DLo & ", " & DHi);
                     Emit_Inst (Ctx.E, SLo & " = cmp_lt_s.i64 " & S64 & ", 0");
                     Emit_Inst (Ctx.E, SHi & " = cmp_gt_s.i64 " & S64 & ", 86400000000000");
                     Emit_Inst (Ctx.E, SB  & " = or.i8 " & SLo & ", " & SHi);
                     Emit_Inst (Ctx.E, B1  & " = or.i8 " & YB & ", " & MB);
                     Emit_Inst (Ctx.E, B2  & " = or.i8 " & DB & ", " & SB);
                     Emit_Inst (Ctx.E, Bad & " = or.i8 " & B1 & ", " & B2);
                     Emit_Check_Fail_If (Ctx, Bad);
                  end;
               end if;
               Emit_Call_Or_Invoke (Ctx, "@__ada_cal_time_of", Y32 & ", " & M32 & ", " & D32 & ", " & S64, "i64", RV);
               Res.Kind := EK_Scalar; Res.Reg := RV;
               Res.Pir_Type := To_Unbounded_String ("i64");
               Res.Ada_Type := Res_Type; Res.Align := 8;
            end;
            return Res;
         elsif Name = "SPLIT" then
            Handled := True;
            declare
               TV : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               YT : constant String := Next_Reg (Ctx.E, "csy");
               MT : constant String := Next_Reg (Ctx.E, "csm");
               DT : constant String := Next_Reg (Ctx.E, "csd");
               ST : constant String := Next_Reg (Ctx.E, "css");
               YV : constant String := Next_Reg (Ctx.E, "csyv");
               MV : constant String := Next_Reg (Ctx.E, "csmv");
               DV : constant String := Next_Reg (Ctx.E, "csdv");
               SV : constant String := Next_Reg (Ctx.E, "cssv");
               Dummy : Unbounded_String;

               type Split_Target is record
                  LV        : Expr_Val;
                  Has_Conv  : Boolean := False;
                  Conv_Typ  : TREE := CONST_VOID;
                  Is_Shared : Boolean := False;
               end record;

               function Prep_Split_Out (Arg_N : TREE) return Split_Target is
                  Inner_N : TREE := Arg_N;
                  Tgt     : Split_Target;
               begin
                  while not Is_Void_Tree (Inner_N) and then KIND (Inner_N) = DN_CONVERSION loop
                     if not Tgt.Has_Conv then
                        Tgt.Has_Conv := True;
                        Tgt.Conv_Typ := Subst_Tree (Ctx, D (SM_EXP_TYPE, Inner_N));
                     end if;
                     Inner_N := D (AS_EXP, Inner_N);
                  end loop;
                  Tgt.Is_Shared := Is_Shared_Object (Ctx, Inner_N);
                  Tgt.LV        := Emit_LValue (Ctx, Inner_N);
                  return Tgt;
               end Prep_Split_Out;

               procedure Store_Split_Out (Tgt : Split_Target; Val_R : String; Src_Pir : String; Src_Typ : TREE) is
                  DTI   : constant Type_Info := Get_Type_Info (Ctx, Tgt.LV.Ada_Type);
                  V_Out : Unbounded_String := To_Unbounded_String (Val_R);
               begin
                  if Tgt.Has_Conv then
                     Emit_Range_Check (Ctx, Val_R, Src_Pir, Tgt.Conv_Typ);
                     V_Out := To_Unbounded_String
                       (Emit_Typed_Scalar_Convert (Ctx, Val_R, Tgt.Conv_Typ, Src_Pir, Tgt.LV.Ada_Type, To_String (DTI.Pir_Type)));
                  elsif To_String (DTI.Pir_Type) /= Src_Pir then
                     V_Out := To_Unbounded_String
                       (Emit_Typed_Scalar_Convert (Ctx, Val_R, Src_Typ, Src_Pir, Tgt.LV.Ada_Type, To_String (DTI.Pir_Type)));
                  end if;
                  Emit_Range_Check (Ctx, To_String (V_Out), To_String (DTI.Pir_Type), Tgt.LV.Ada_Type);
                  if Tgt.Is_Shared then
                     Emit_Inst (Ctx.E, "fence [seq_cst]");
                     Emit_Inst (Ctx.E,
                       "store." & To_String (DTI.Pir_Type) & " " & To_String (V_Out) &
                       ", " & To_String (Tgt.LV.Reg) & ", align " & Img (DTI.Align) & " [volatile]");
                     Emit_Inst (Ctx.E, "fence [seq_cst]");
                  else
                     Emit_Inst (Ctx.E,
                       "store." & To_String (DTI.Pir_Type) & " " & To_String (V_Out) &
                       ", " & To_String (Tgt.LV.Reg) & ", align " & Img (DTI.Align));
                  end if;
               end Store_Split_Out;

               Tgt_Y, Tgt_M, Tgt_D, Tgt_S : Split_Target;
            begin
               -- P3-4.16: Evaluate all out actual LValues BEFORE calling @__ada_cal_split (LRM 6.4.1(2))
               Tgt_Y := Prep_Split_Out (Args (2));
               Tgt_M := Prep_Split_Out (Args (3));
               Tgt_D := Prep_Split_Out (Args (4));
               Tgt_S := Prep_Split_Out (Args (5));
               Emit_Entry_Alloca (Ctx.E, YT & " = alloca i32, align 4");
               Emit_Entry_Alloca (Ctx.E, MT & " = alloca i32, align 4");
               Emit_Entry_Alloca (Ctx.E, DT & " = alloca i32, align 4");
               Emit_Entry_Alloca (Ctx.E, ST & " = alloca i64, align 8");
               Emit_Call_Or_Invoke (Ctx, "@__ada_cal_split",
                 To_String (TV.Reg) & ", " & YT & ", " & MT & ", " & DT & ", " & ST, "", Dummy);
               Emit_Inst (Ctx.E, YV & " = load.i32 " & YT & ", align 4");
               Emit_Inst (Ctx.E, MV & " = load.i32 " & MT & ", align 4");
               Emit_Inst (Ctx.E, DV & " = load.i32 " & DT & ", align 4");
               Emit_Inst (Ctx.E, SV & " = load.i64 " & ST & ", align 8");
               Store_Split_Out (Tgt_Y, YV, "i32", D (SM_EXP_TYPE, Args (2)));
               Store_Split_Out (Tgt_M, MV, "i32", D (SM_EXP_TYPE, Args (3)));
               Store_Split_Out (Tgt_D, DV, "i32", D (SM_EXP_TYPE, Args (4)));
               Store_Split_Out (Tgt_S, SV, "i64", D (SM_EXP_TYPE, Args (5)));
            end;
            return Res;
         elsif Name = """-""" or else Name = """+""" or else Name = """<"""
           or else Name = """<=""" or else Name = """>""" or else Name = """>="""
         then
            Handled := True;
            declare
               L  : constant Expr_Val := Emit_Expr (Ctx, Args (1));
               R_V : constant Expr_Val := Emit_Expr (Ctx, Args (2));
               Out_R : Unbounded_String;
            begin
               Res.Kind := EK_Scalar;
               Res.Ada_Type := Res_Type;
               if Name = """-""" then
                  if Is_Fixed_Type (Res_Type) then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_sub_time",
                       To_String (L.Reg) & ", " & To_String (R_V.Reg), "i64", Out_R);
                  else
                     if Ctx.Enable_Checks then
                        declare
                           LoC : constant String := Next_Reg (Ctx.E, "cd1");
                           HiC : constant String := Next_Reg (Ctx.E, "cd2");
                           Bad : constant String := Next_Reg (Ctx.E, "cdb");
                        begin
                           Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i64 " & To_String (R_V.Reg) & ", -86400000000000");
                           Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i64 " & To_String (R_V.Reg) & ", 86400000000000");
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end;
                     end if;
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_sub_dur",
                       To_String (L.Reg) & ", " & To_String (R_V.Reg), "i64", Out_R);
                  end if;
                  Res.Pir_Type := To_Unbounded_String ("i64");
               elsif Name = """+""" then
                  declare
                     Dur_R : constant String := (if Is_Fixed_Type (L.Ada_Type) then To_String (L.Reg) else To_String (R_V.Reg));
                  begin
                     if Ctx.Enable_Checks then
                        declare
                           LoC : constant String := Next_Reg (Ctx.E, "cp1");
                           HiC : constant String := Next_Reg (Ctx.E, "cp2");
                           Bad : constant String := Next_Reg (Ctx.E, "cpb");
                        begin
                           Emit_Inst (Ctx.E, LoC & " = cmp_lt_s.i64 " & Dur_R & ", -86400000000000");
                           Emit_Inst (Ctx.E, HiC & " = cmp_gt_s.i64 " & Dur_R & ", 86400000000000");
                           Emit_Inst (Ctx.E, Bad & " = or.i8 " & LoC & ", " & HiC);
                           Emit_Check_Fail_If (Ctx, Bad);
                        end;
                     end if;
                  end;
                  if Is_Fixed_Type (L.Ada_Type) then
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_add",
                       To_String (R_V.Reg) & ", " & To_String (L.Reg), "i64", Out_R);
                  else
                     Emit_Call_Or_Invoke (Ctx, "@__ada_cal_add",
                       To_String (L.Reg) & ", " & To_String (R_V.Reg), "i64", Out_R);
                  end if;
                  Res.Pir_Type := To_Unbounded_String ("i64");
               else
                  declare
                     CR : constant String := Next_Reg (Ctx.E, "cal");
                  begin
                     if Name = """<""" then
                        Emit_Inst (Ctx.E, CR & " = cmp_lt_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     elsif Name = """<=""" then
                        Emit_Inst (Ctx.E, CR & " = cmp_le_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     elsif Name = """>""" then
                        Emit_Inst (Ctx.E, CR & " = cmp_gt_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     else
                        Emit_Inst (Ctx.E, CR & " = cmp_ge_s.i64 " & To_String (L.Reg) & ", " & To_String (R_V.Reg));
                     end if;
                     Out_R := To_Unbounded_String (CR);
                  end;
                  Res.Pir_Type := To_Unbounded_String ("i8");
               end if;
               Res.Reg := Out_R;
               return Res;
            end;
         end if;
      end if;
      end;
      return Res;
   end Try_Emit_RTL_Call;

end Cg_Expr.RTL;
