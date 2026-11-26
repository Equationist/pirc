with TEXT_IO, Multi_Sep_Pkg;
use TEXT_IO, Multi_Sep_Pkg;
pragma ELABORATE (Multi_Sep_Pkg);

procedure Multi_Sep_Main is
   type Der_Box is new Lim_Box;
   Caught : Boolean := False;
   V1, V2 : Integer := 0;
begin
   -- Cross-unit deferred constant
   if Read_Const /= 777 then
      raise Program_Error;
   end if;
   -- Cross-unit derived implicit "/=" (P3-7.7)
   if not (Der_Box'(Make_Box (5)) /= Der_Box'(Make_Box (6)))
     or else (Der_Box'(Make_Box (5)) /= Der_Box'(Make_Box (5)))
   then
      raise Program_Error;
   end if;
   -- Cross-unit object renaming
   Ren_Var := 25;
   if Base_Var /= 25 or else Sub_Add (3, 4) /= 32 then
      raise Program_Error;
   end if;
   -- Cross-unit library 'Storage_Size (P3-6.6)
   if Pool_Acc'Storage_Size /= 2048 then
      raise Program_Error;
   end if;
   -- Cross-unit renamed entry call + timed entry call (P3-4.11)
   Srv_Give (V1);
   if V1 /= 321 then
      raise Program_Error;
   end if;
   V1 := 0;
   select
      Srv.Give (V1);
   or
      delay 1.0;
      raise Program_Error;
   end select;
   if V1 /= 321 then
      raise Program_Error;
   end if;
   -- Separate task body subunit (P3-5.17)
   Sub_Task.Ping (10, V2);
   if V2 /= 787 then
      raise Program_Error;
   end if;
   -- Cross-unit user exception raised from subunit (P3-5.10)
   begin
      Raise_From_Sub;
   exception
      when My_Cross_Exc => Caught := True;
   end;
   if not Caught then
      raise Program_Error;
   end if;
   Put_Line ("MULTI_SEP_OK");
end Multi_Sep_Main;
