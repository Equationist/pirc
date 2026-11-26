-- Negative test: P3-7.15 task subtype prefix for 'Terminated inside task body
procedure Neg_Task_Sub_Terminated is
   task type T;
   subtype T_Sub is T;
   task body T is
      B : Boolean := T_Sub'Terminated;
   begin
      if B then null; end if;
   end T;
begin
   null;
end Neg_Task_Sub_Terminated;
