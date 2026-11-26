-- Multi-unit separate compilation regression package spec:
-- Tests cross-unit user exception, deferred scalar constant, object renaming,
-- entry renaming (called via conditional/timed entry call), derived "=" & "/=",
-- library 'Storage_Size, and separate subprogram & task subunits.
package Multi_Sep_Pkg is
   My_Cross_Exc : exception;

   type Priv_Num is private;
   Def_Const : constant Priv_Num;

   type Lim_Box is limited private;
   function Make_Box (V : Integer) return Lim_Box;
   function "=" (L, R : Lim_Box) return Boolean;

   Base_Var : Integer := 10;
   Ren_Var  : Integer renames Base_Var;

   type Pool_Acc is access Integer;
   for Pool_Acc'Storage_Size use 2048;

   task Srv is
      entry Give (V : out Integer);
   end Srv;
   procedure Srv_Give (V : out Integer) renames Srv.Give;

   function Read_Const return Integer;
   function Sub_Add (A, B : Integer) return Integer;
   procedure Raise_From_Sub;

   task Sub_Task is
      entry Ping (X : Integer; Y : out Integer);
   end Sub_Task;

private
   type Priv_Num is new Integer;
   Def_Const : constant Priv_Num := 777;
   type Lim_Box is new Integer;
end Multi_Sep_Pkg;
