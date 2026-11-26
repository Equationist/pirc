-- Pass 3 Stage 7 Regression Test: Valid Ada 83 edge cases fixed in P3-7 Parts A & B:
--   P3-7.1:  Multi-choice array aggregate with subtype mark (1..2 | Sub => 10, others => 0)
--   P3-7.2:  2-level selected 1-arg attribute prefix (P.Sub.F(1)'Length)
--   P3-7.3:  Generic constant renaming, operator renaming attribute, self-renamed generic pkg + use
--   P3-7.4:  A .. A => 10 in enum rep clause & P.T'Size expanded name length clause
--   P3-7.6:  Overloaded enclosing subps (P.X), private/body selection via pkg rename, non-ovl hiding derived op
--   P3-7.7:  Named+others aggregates in const/var/assign/return/default; derived op in generic pkg;
--            predefined "=" on record with private component in private part
--   P3-7.8:  Lowercase operator named generic assoc ("and" => ...); non-static non-discr in out comp;
--            differently grouped formal/actual discriminants; formal "=" providing implicit "/="
--   P3-7.9:  Overloaded parameterless function attribute prefix (F'First, F'Constrained);
--            Dyn'Base'Pos/'Pred/'Succ/'Val static; T_Type.E'Count inside task body T_Type
--   P3-7.10: Derived fixed type for Der_F'Small checked against derived delta
with TEXT_IO;
use TEXT_IO;

procedure Pass3_Stage7_Valid_Test is

   procedure Fail (Msg : String) is
   begin
      Put_Line ("FAIL P3S7: " & Msg);
      raise Program_Error;
   end Fail;

   -- P3-7.2: 2-level selected 1-arg attribute prefix
   package P2_Outer is
      package Sub is
         function F (N : Integer) return String;
      end Sub;
   end P2_Outer;
   package body P2_Outer is
      package body Sub is
         function F (N : Integer) return String is
            S : String (1 .. N) := (others => 'a');
         begin
            return S;
         end F;
      end Sub;
   end P2_Outer;

   -- P3-7.3: Generic constant renaming & self-renamed generic package + use Self
   generic
      K : Integer;
   package Self_Ren_Gen is
      package Self renames Self_Ren_Gen;
      use Self;
      C_Orig : constant Integer := K + 1;
      C_Ren  : Integer renames C_Orig;
      function Get return Integer;
   end Self_Ren_Gen;
   package body Self_Ren_Gen is
      function Get return Integer is begin return C_Ren; end Get;
   end Self_Ren_Gen;

   -- P3-7.6 & P3-7.7: Enclosing package renaming selecting private/body items,
   -- and predefined "=" on record with private component in private part
   package Priv_Eq_Pkg is
      type Priv is private;
      type Wrap is record
         P : Priv;
      end record;
      function Check_Eq return Boolean;
   private
      type Priv is new Integer;
      C1 : constant Wrap := (P => 5);
      C2 : constant Wrap := (P => 5);
      Same_In_Private : constant Boolean := (C1 = C2);
   end Priv_Eq_Pkg;
   package body Priv_Eq_Pkg is
      package P_Alias renames Priv_Eq_Pkg;
      Body_Secret : constant Integer := 42;
      function Check_Eq return Boolean is
      begin
         return P_Alias.Same_In_Private and then (P_Alias.Body_Secret = 42);
      end Check_Eq;
   end Priv_Eq_Pkg;

   -- P3-7.7: Derived type inside generic package inheriting parent op
   generic
   package Der_In_Gen is
      type T1 is range 1 .. 100;
      function Double (X : T1) return T1;
      function Run return Integer;
   end Der_In_Gen;
   package body Der_In_Gen is
      type T2 is new T1;
      function Double (X : T1) return T1 is begin return X * 2; end Double;
      function Run return Integer is
         V : T2 := Double (T2 (21));
      begin
         return Integer (V);
      end Run;
   end Der_In_Gen;

   -- P3-7.8: Lowercase operator named assoc, differently grouped discriminants, formal "=" -> "/="
   package Lim_Op_Pkg is
      type Lim_Int is limited private;
      function Mk (V : Integer) return Lim_Int;
      function "=" (L, R : Lim_Int) return Boolean;
   private
      type Lim_Int is new Integer;
   end Lim_Op_Pkg;
   package body Lim_Op_Pkg is
      function Mk (V : Integer) return Lim_Int is begin return Lim_Int (V); end Mk;
      function "=" (L, R : Lim_Int) return Boolean is begin return Integer (L) = Integer (R); end "=";
   end Lim_Op_Pkg;

   generic
      type Lim_T is limited private;
      with function "AND" (L, R : Integer) return Integer;
      with function "=" (L, R : Lim_T) return Boolean is <>;
   package Op_Gen is
      function Test_Ops (A, B : Integer; X, Y : Lim_T) return Integer;
   end Op_Gen;
   package body Op_Gen is
      function Test_Ops (A, B : Integer; X, Y : Lim_T) return Integer is
      begin
         if X /= Y then
            return A and B;
         end if;
         return 0;
      end Test_Ops;
   end Op_Gen;

   generic
      type R_T (A, B : Integer) is private;
   package Grp_Disc_Gen is
      function Sum_D (X : R_T) return Integer;
   end Grp_Disc_Gen;
   package body Grp_Disc_Gen is
      function Sum_D (X : R_T) return Integer is
      begin
         return X.A + X.B;
      end Sum_D;
   end Grp_Disc_Gen;

   procedure Test_All is
      -- P3-7.2: Invalid pragma PRIORITY("high") must be ignored without ICE (LRM 2.8(7))
      pragma PRIORITY ("high");

      -- P3-7.1: Multi-choice array aggregate with subtype mark
      subtype Sub34 is Integer range 3 .. 4;
      type A5 is array (1 .. 5) of Integer;
      Arr_MC : A5 := (1 .. 2 | Sub34 => 10, others => 99);

      -- P3-7.3: Operator renaming attribute
      function "+" (X : Integer) return Integer renames Integer'Succ;
      X41 : Integer := 41;

      -- P3-7.4: A .. A => 10 in enum rep & expanded name P.T'Size
      type Small_E is (EA, EB);
      for Small_E use (EA .. EA => 10, EB => 20);
      type Exp_Sz_T is range 0 .. 15;
      for Test_All.Exp_Sz_T'Size use 8;

      -- P3-7.6: Explicit non-overloadable variable hiding implicit derived subprogram
      package Der_Hide_Base is
         type B is range 1 .. 10;
         function Foo (X : B) return B;
      end Der_Hide_Base;

      -- P3-7.7: Named+others aggregates in const, var, assign, return, default param
      C_Agg : constant A5 := (1 => 10, others => 2);
      V_Agg : A5 := (2 => 20, others => 3);

      -- P3-7.8: Non-static non-discriminant in out component
      Dyn_N : Integer := 3;
      type Int_Arr is array (Positive range <>) of Integer;
      type Rec_Dyn (D : Integer := 1) is record
         A : Int_Arr (1 .. Dyn_N) := (others => 7);
      end record;
      generic
         Target : in out Int_Arr;
      procedure Bump_Arr;

      -- P3-7.8: Differently grouped discriminants
      type Act_Rec (A : Integer; B : Integer) is record
         V : Integer := 0;
      end record;
      package GDG is new Grp_Disc_Gen (Act_Rec);
      AR : Act_Rec (3, 4);

      function My_And (L, R : Integer) return Integer;
      use Lim_Op_Pkg;
      package OG is new Op_Gen (Lim_Op_Pkg.Lim_Int, "and" => My_And);
      package SRG is new Self_Ren_Gen (41);
      package DIG is new Der_In_Gen;

      -- P3-7.9: Overloaded parameterless function attribute prefix & 'Constrained
      type Rec_D (D : Integer := 1) is record
         X : Integer := 0;
      end record;

      -- P3-7.9: Dyn'Base'Pos/'Pred/'Succ/'Val static
      subtype Dyn_Sub is Integer range 1 .. Dyn_N;
      Static_P : constant := Dyn_Sub'Base'Pos (6);
      Static_S : constant Integer := Dyn_Sub'Base'Succ (5);

      -- P3-7.9: T_Type.E'Count inside task body T_Type
      task type Cnt_Task is
         entry E (N : out Integer);
      end Cnt_Task;
      CT : Cnt_Task;
      EC : Integer := -1;

      -- P3-7.10: Derived fixed type for Der_F'Small checked against derived delta (>= parent delta)
      type Par_Fix is delta 0.25 range -10.0 .. 10.0;
      type Der_Fix is new Par_Fix delta 0.5 range -10.0 .. 10.0;
      for Der_Fix'Small use 0.5;
      DF : Der_Fix := 1.5;

      -- Later declarative items (bodies)
      procedure Ov_P (Dummy : Integer) is
         Only_In_Int : Integer := 77;
         procedure Ov_P (Dummy2 : Boolean) is
            Only_In_Bool : Integer := 88;
         begin
            if Dummy = 0 or else Dummy2 then null; end if;
            if Ov_P.Only_In_Int /= 77 or else Ov_P.Only_In_Bool /= 88 then
               Fail ("Overloaded enclosing subprogram expanded name");
            end if;
         end Ov_P;
      begin
         Ov_P (True);
      end Ov_P;

      package body Der_Hide_Base is
         function Foo (X : B) return B is begin return X; end Foo;
      end Der_Hide_Base;

      function Ret_Agg (D : A5 := (5 => 50, others => 1)) return A5 is
      begin
         if D (5) /= 50 or else D (1) /= 1 then
            Fail ("Default param named+others aggregate");
         end if;
         return (3 => 30, others => 4);
      end Ret_Agg;

      procedure Bump_Arr is
      begin
         Target (1) := Target (1) + 1;
      end Bump_Arr;

      function My_And (L, R : Integer) return Integer is
      begin
         return L + R;
      end My_And;

      function Get_Arr return A5 is begin return (1, 2, 3, 4, 5); end Get_Arr;
      function Get_Arr (Dummy : Integer) return A5 is begin return (others => Dummy); end Get_Arr;
      function Get_Rec return Rec_D is begin return (D => 1, X => 9); end Get_Rec;
      function Get_Rec (Dummy : Integer) return Rec_D is begin return (D => Dummy, X => 0); end Get_Rec;

      task body Cnt_Task is
      begin
         select
            accept E (N : out Integer) do
               N := Cnt_Task.E'Count;
            end E;
         or
            terminate;
         end select;
      end Cnt_Task;
   begin
      if Arr_MC (1) /= 10 or else Arr_MC (3) /= 10 or else Arr_MC (5) /= 99 then
         Fail ("P3-7.1 multi-choice subtype aggregate");
      end if;
      if P2_Outer.Sub.F (4)'Length /= 4 then
         Fail ("P3-7.2 2-level selected 1-arg attr prefix");
      end if;
      if (+X41) /= 42 then Fail ("P3-7.3 operator renaming attribute"); end if;
      if SRG.Get /= 42 then Fail ("P3-7.3 self-renamed generic pkg"); end if;
      if Small_E'Pos (EB) /= 1 or else Exp_Sz_T'Size /= 8 then
         Fail ("P3-7.4 enum rep range / expanded name 'Size");
      end if;

      Ov_P (1);
      if not Priv_Eq_Pkg.Check_Eq then Fail ("P3-7.6/7 private eq & pkg rename"); end if;

      -- Explicit variable hiding derived subprogram Foo
      declare
         type D_B is new Der_Hide_Base.B;
         Foo : Integer := 55;
      begin
         if Foo /= 55 or else D_B'First /= 1 then
            Fail ("P3-7.6 non-overloadable hiding derived op");
         end if;
      end;

      V_Agg := (4 => 40, others => 5);
      if C_Agg (1) /= 10 or else C_Agg (5) /= 2 or else V_Agg (4) /= 40 or else Ret_Agg (3) /= 30 then
         Fail ("P3-7.7 named+others aggregates");
      end if;
      if DIG.Run /= 42 then Fail ("P3-7.7 derived op in generic pkg"); end if;

      if OG.Test_Ops (10, 20, Lim_Op_Pkg.Mk (1), Lim_Op_Pkg.Mk (2)) /= 30 then Fail ("P3-7.8 lowercase op assoc & formal /="); end if;
      if GDG.Sum_D (AR) /= 7 then Fail ("P3-7.8 differently grouped discriminants"); end if;

      declare
         RD : Rec_Dyn;
         procedure Do_Bump is new Bump_Arr (RD.A);
      begin
         Do_Bump;
         if RD.A (1) /= 8 then Fail ("P3-7.8 non-static non-discr in out comp"); end if;
      end;

      if Get_Arr'First /= 1 or else not Get_Rec'Constrained then
         Fail ("P3-7.9 overloaded paramless func attr prefix");
      end if;
      if Static_P /= 6 or else Static_S /= 6 then Fail ("P3-7.9 Dyn'Base'Pos/Succ static"); end if;

      CT.E (EC);
      if EC /= 0 then Fail ("P3-7.9 T_Type.E'Count"); end if;

      if DF /= 1.5 or else Der_Fix'Small /= 0.5 then
         Fail ("P3-7.10 derived fixed 'Small");
      end if;
   end Test_All;

begin
   Test_All;
   Put_Line ("PASS3_STAGE7_VALID_OK");
end Pass3_Stage7_Valid_Test;
