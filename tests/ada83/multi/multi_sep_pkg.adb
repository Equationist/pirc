package body Multi_Sep_Pkg is
   function Make_Box (V : Integer) return Lim_Box is
   begin
      return Lim_Box (V);
   end Make_Box;

   function "=" (L, R : Lim_Box) return Boolean is
   begin
      return Integer (L) = Integer (R);
   end "=";

   function Read_Const return Integer is
   begin
      return Integer (Def_Const);
   end Read_Const;

   task body Srv is
   begin
      loop
         select
            accept Give (V : out Integer) do
               V := 321;
            end Give;
         or
            terminate;
         end select;
      end loop;
   end Srv;

   function Sub_Add (A, B : Integer) return Integer is separate;
   procedure Raise_From_Sub is separate;
   task body Sub_Task is separate;
end Multi_Sep_Pkg;
