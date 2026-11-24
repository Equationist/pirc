with Text_IO;
with Calendar;
procedure Test07_Strings is
   package Int_IO is new Text_IO.Integer_IO(Integer);
   S1 : constant String := "Hello";
   S2 : constant String := "Ada83";
   S3 : constant String := S1 & ", " & S2 & '!';
   T0, T1 : Calendar.Time;
   Dt : Duration;
   Sec : Long_Float;
begin
   Text_IO.Put_Line(S3);
   Text_IO.Put("len="); Int_IO.Put(S3'Length, Width => 0); Text_IO.New_Line;
   Text_IO.Put("sub="); Text_IO.Put_Line(S3(8 .. 12));
   if S3(1 .. 5) = "Hello" then
      Text_IO.Put_Line("match=1");
   end if;
   T0 := Calendar.Clock;
   T1 := Calendar.Clock;
   Dt := Calendar."-"(T1, T0);
   Sec := Long_Float(Dt);
   if Sec >= 0.0 and then Sec < 10.0 then
      Text_IO.Put_Line("clock=ok");
   end if;
end Test07_Strings;
